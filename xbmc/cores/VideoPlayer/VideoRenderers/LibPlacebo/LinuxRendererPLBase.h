/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// NOTE: This header must be included AFTER the concrete base renderer header
// (LinuxRendererGL.h or LinuxRendererGLES.h) so that renderer-specific types
// such as CPictureBuffer, RenderMethod, and EShaderFormat are already declared.

#include "PLHelper.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/DRMPRIMEEGL.h"
#include "cores/VideoPlayer/VideoRenderers/VideoShaders/ShaderFormats.h"
#include "filesystem/File.h"
#include "settings/Settings.h"
#include "utils/HDRCapabilities.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include "system_gl.h"

#include <libplacebo/opengl.h>

#if defined(HAVE_LIBVA)
#include "cores/VideoPlayer/DVDCodecs/Video/VAAPI.h"

#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <va/va_drmcommon.h>
#endif // HAVE_LIBVA

extern "C"
{
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <array>
#include <cstring>

#include <libplacebo/shaders/icc.h>
#include <libplacebo/utils/libav.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

// GL_R16 / GL_RG16 / GL_RGBA16 are desktop GL core.  GLES exposes them via
// GL_EXT_texture_norm16 under the _EXT suffix but with identical enum values.
// Define the un-suffixed names as fallbacks so the same code compiles on both.
#ifndef GL_R16
#define GL_R16 0x822A
#endif
#ifndef GL_RG16
#define GL_RG16 0x822C
#endif
#ifndef GL_RGBA16
#define GL_RGBA16 0x805B
#endif
#ifndef GL_UNSIGNED_NORMALIZED
#define GL_UNSIGNED_NORMALIZED 0x8C17
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE 0x8211
#endif

/**
 * @brief CRTP mixin providing the shared libplacebo renderer implementation.
 *
 * TBase is either CLinuxRendererGL or CLinuxRendererGLES.
 * Concrete classes must:
 *   - Provide static Create()/Register() factory methods
 *   - Implement GetVaapiTexTarget() — returns GL_TEXTURE_2D (desktop) or
 *     GL_TEXTURE_EXTERNAL_OES (GLES)
 *   - Implement GetDRMPRIMEEGLDisplay() — returns the platform EGLDisplay to
 *     pass to CDRMPRIMETexture::Init()
 */
template<typename TBase>
class CLinuxRendererPLBase : public TBase
{
public:
  CLinuxRendererPLBase();
  ~CLinuxRendererPLBase() override;

  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation) override;
  bool ConfigChanged(const VideoPicture& picture) override;
  [[nodiscard]] bool Supports(ERENDERFEATURE feature) const override;
  [[nodiscard]] bool Supports(ESCALINGMETHOD method) const override;
  CRenderInfo GetRenderInfo() override;
  void AddVideoPicture(const VideoPicture& picture, int index) override;
  bool Flush(bool saveBuffers) override;

protected:
  bool CreateTexture(int index) override;
  void DeleteTexture(int index) override;
  bool UploadTexture(int index) override;
  bool LoadShadersHook() override;
  bool RenderHook(int idx) override;
  void UpdateVideoFilter() override;
  EShaderFormat GetShaderFormat() override;

  // Platform-specific hooks implemented by concrete subclasses.
  [[nodiscard]] virtual GLenum GetVaapiTexTarget() const = 0;
  [[nodiscard]] virtual EGLDisplay GetDRMPRIMEEGLDisplay() const = 0;

private:
  // Persistent per-slot GL resources for the software decode upload path.
  // Kept separate from PLBuffer so they survive across per-frame ReleasePLBuffer
  // calls (which only destroy the lightweight pl_tex wrappers). Freed in
  // DeleteTexture and the destructor.
  struct SWBuffer
  {
    GLuint pbo[4]{0, 0, 0, 0}; ///< Pixel Buffer Objects for async CPU→GPU DMA
    GLsizeiptr pboSize[4]{0, 0, 0, 0}; ///< Allocated PBO size (bytes) for resize detection
    GLuint tex[4]{0, 0, 0, 0}; ///< Target GL_TEXTURE_2D textures
    int texW[4]{0, 0, 0, 0}; ///< Cached dimensions for resize detection
    int texH[4]{0, 0, 0, 0};
    GLenum texIformat[4]{0, 0, 0, 0}; ///< Cached iformat for format-change detection
  };

  struct PLBuffer
  {
    pl_plane planes[4]{};
    pl_tex tex[4]{};
    int num_planes{0};
    pl_color_space colorSpace{};
    pl_color_repr colorRepr{};
    pl_dovi_metadata doviMetadata{}; ///< Owned copy; colorRepr.dovi points here when valid
    unsigned int iFlags{0}; ///< DVP_FLAG_* interlace flags
    bool loaded{false};
    
    // Pending EGL sync object imported from a DMA-buf sync-file fence.
    // EGL_NO_SYNC_KHR means eglWaitSyncKHR has already been called or no sync is needed.
    // Used by both the VAAPI and DRMPRIME paths.
    EGLSyncKHR eglSyncFence{EGL_NO_SYNC_KHR};
    
#if defined(HAVE_LIBVA)
    GLuint vaapiGLTex[3]{};
    EGLImageKHR vaapiEGLImage[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    int vaapiExportedFd[4]{-1, -1, -1, -1};
    int vaapiNumFds{0};
#endif
  };

  std::array<PLBuffer, NUM_BUFFERS> m_plBuffers{};
  std::array<SWBuffer, NUM_BUFFERS> m_swBuffers{};
  std::array<CDRMPRIMETexture, NUM_BUFFERS> m_drmTextures{};

  // Cached pl_tex wrapper for each DRMPRIME slot.
  // pl_opengl_wrap + pl_tex_destroy per frame costs a glGetError() stall (via
  // gl_check_err inside libplacebo) plus mutex round-trips and heap allocation.
  // The GL texture ID and dimensions are stable across frames for a given slot,
  // so the wrapper can be reused. Destroyed in DeleteTexture and the destructor.
  struct DRMTexCache
  {
    pl_tex tex{nullptr};
    GLuint glTex{0};
    int width{0};
    int height{0};
  };
  std::array<DRMTexCache, NUM_BUFFERS> m_drmTexCache{};

  bool m_isDRMPRIME{false};
  // True when eglQueryDmaBufModifiersEXT is available: the EGL stack inserts
  // implicit DMA-buf fences at eglCreateImageKHR time, making explicit CPU or
  // GPU-side sync redundant for both VAAPI and DRMPRIME paths.
  bool m_hasEGLModifiers{false};

  // EGL context used globally for Sync objects and Image KHR
  EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};

#if defined(HAVE_LIBVA)
  bool m_isVAAPI{false};
  PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR{nullptr};
  PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR{nullptr};
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES{nullptr};
#endif

  // GL_EXT_EGL_image_storage: immutable-format texture from EGLImage.
  // The driver can skip per-frame format validation and pre-compute tiling
  // metadata, reducing CPU overhead on tile-based GPUs (V3D/RPi5, Mali, etc.).
  PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC m_glEGLImageTargetTexStorageEXT{nullptr};
  bool m_hasEGLImageStorage{false};

  AVPixelFormat m_format{AV_PIX_FMT_NONE};
  pl_color_space m_colorSpace{};
  pl_chroma_location m_chromaLocation{PL_CHROMA_UNKNOWN};
  std::unique_ptr<PL::RenderConfig> m_plConfig;
  CHDRCapabilities m_displayHDRCaps;

  struct QueuedFrameState
  {
    int bufferIndex;
    CLinuxRendererPLBase* renderer;
  };
  // libplacebo guarantees exactly one of unmap/discard is called per pushed frame.
  // QueuedFrameState must be trivially destructible so raw delete is safe and
  // no destructor side effects are silently skipped on the discard path.
  static_assert(std::is_trivially_destructible_v<QueuedFrameState>);

  pl_queue m_plQueue{nullptr};
  double m_queuePtsOffset{0.0};
  bool m_queuePtsOffsetSet{false};

  // Cached GL framebuffer → pl_tex wrapper.
  // pl_opengl_wrap/pl_tex_destroy for a framebuffer flushes GPU command queues on
  // some drivers and forces a full libplacebo internal state reset.  Wrapping once
  // and reusing the same pl_tex across frames avoids that overhead entirely.
  // Invalidated when the bound FBO id, viewport width, or viewport height changes.
  pl_tex m_cachedFboTex{nullptr};
  unsigned int m_cachedFboId{UINT_MAX};
  int m_cachedFboW{0};
  int m_cachedFboH{0};

  // Cached Kodi VAO handle.
  // glGetIntegerv(GL_VERTEX_ARRAY_BINDING) serializes the GPU command stream on
  // tile-based GPUs (Mali / Adreno / PowerVR / Apple GPU) because the driver must
  // finish all in-flight work before reading back a GPU-side integer.  Kodi's VAO
  // is stable for the lifetime of the renderer, so we query it once and cache it.
  GLint m_kodiVAO{0};
  bool m_kodiVaoCached{false};

  // Cached GL save/restore state.
  // Viewport, scissor enable, and scissor box are CPU-side shadow state in every
  // known GL driver (not a GPU round-trip), but we still save 3 function calls per
  // frame by caching them.  Invalidated whenever the FBO dimensions change.
  bool m_glStateCached{false};
  GLint m_cachedGLViewport[4]{};
  GLboolean m_cachedScissorEnabled{GL_FALSE};
  GLint m_cachedGLScissor[4]{};

  // When the GL context was created with EGL_CONTEXT_OPENGL_NO_ERROR_KHR, all
  // error generation is disabled and glGetError() always returns GL_NO_ERROR.
  // In that case we can skip the pre-render error drain entirely.
  bool m_glNoError{false};

  // EGL_ANDROID_native_fence_sync: GPU-side DMA-buf fence wait.
  //
  // Used for both VAAPI and DRMPRIME paths.  When this extension and the
  // DMA_BUF_IOCTL_EXPORT_SYNC_FILE ioctl (kernel ≥ 5.2) are available, we
  // export the DMA-buf read fence as a sync-file fd, import it as an EGL
  // sync object, and call eglWaitSyncKHR — a GPU command-stream wait that
  // never stalls the CPU.
  PFNEGLCREATESYNCKHRPROC m_eglCreateSyncKHR{nullptr};
  PFNEGLDESTROYSYNCKHRPROC m_eglDestroySyncKHR{nullptr};
  PFNEGLWAITSYNCKHRPROC m_eglWaitSyncKHR{nullptr};
  bool m_hasEGLSyncFence{false};

  bool UploadVAAPI(int index, PLBuffer& plbuf);
  bool UploadDRMPRIME(int index, PLBuffer& plbuf);
  bool UploadSoftware(int index, PLBuffer& plbuf);

  static bool MapCallback(pl_gpu gpu,
                          pl_tex* tex,
                          const struct pl_source_frame* src,
                          struct pl_frame* out);
  static void UnmapCallback(pl_gpu gpu, struct pl_frame* frame, const struct pl_source_frame* src);
  static void DiscardCallback(const struct pl_source_frame* src);

  void ReleasePLBuffer(int index);
  void ReleaseSWBuffer(int index);

  // Map pl_plane_data component layout → GL iformat/format/type/bytesPerPixel.
  static bool PlaneDataToGLFormats(
      const pl_plane_data& pd, GLenum& iformat, GLenum& format, GLenum& type, int& bytesPerPixel);
};

// =============================================================================
// Template implementations
// =============================================================================

template<typename TBase>
CLinuxRendererPLBase<TBase>::CLinuxRendererPLBase()
{
  m_plConfig = std::make_unique<PL::RenderConfig>();
}

template<typename TBase>
CLinuxRendererPLBase<TBase>::~CLinuxRendererPLBase()
{
  for (int i = 0; i < NUM_BUFFERS; ++i)
  {
    ReleasePLBuffer(i);
    ReleaseSWBuffer(i);
  }
  pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  for (auto& cache : m_drmTexCache)
  {
    if (cache.tex)
      pl_tex_destroy(gpu, &cache.tex);
    cache = {};
  }
  if (m_plQueue)
  {
    pl_queue_destroy(&m_plQueue);
    m_plQueue = nullptr;
  }
  if (m_cachedFboTex)
  {
    pl_tex_destroy(gpu, &m_cachedFboTex);
    m_cachedFboTex = nullptr;
  }
  m_plConfig.reset();
  PL::PLInstance::Get()->Reset();
}

// ---------------------------------------------------------------------------
// Configure / Flush / AddVideoPicture
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::Configure(const VideoPicture& picture,
                                            float fps,
                                            unsigned int orientation)
{
  if (!TBase::Configure(picture, fps, orientation))
    return false;

  m_format = picture.videoBuffer->GetFormat();

  m_colorSpace = pl_color_space{};
  m_colorSpace.primaries = pl_primaries_from_av(picture.color_primaries);
  m_colorSpace.transfer = pl_transfer_from_av(picture.color_transfer);
  m_chromaLocation = pl_chroma_from_av(picture.chroma_position);

  m_eglDisplay = eglGetCurrentDisplay();
  if (m_eglDisplay == EGL_NO_DISPLAY)
    m_eglDisplay = GetDRMPRIMEEGLDisplay();

  // Probe EGL DMA-buf modifier support unconditionally: applies to both VAAPI
  // and DRMPRIME.  When present, eglCreateImageKHR implicitly attaches the
  // DMA-buf reservation fence to the EGLImage so the GPU waits automatically —
  // no explicit CPU stall or explicit sync is needed.
  m_hasEGLModifiers = (eglGetProcAddress("eglQueryDmaBufModifiersEXT") != nullptr);

  // Probe GL_EXT_EGL_image_storage: immutable-format texture from EGLImage.
  // Avoids per-frame format validation overhead vs glEGLImageTargetTexture2DOES.
  {
    auto* fn = eglGetProcAddress("glEGLImageTargetTexStorageEXT");
    if (fn)
    {
      m_glEGLImageTargetTexStorageEXT =
          reinterpret_cast<PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC>(fn);
      m_hasEGLImageStorage = true;
    }
  }

#if defined(HAVE_LIBVA)
  m_isVAAPI = (dynamic_cast<VAAPI::CVaapiRenderPicture*>(picture.videoBuffer) != nullptr);
  if (m_isVAAPI)
  {
    m_eglCreateImageKHR =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    m_eglDestroyImageKHR =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    m_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  }
#endif

  m_isDRMPRIME = (dynamic_cast<CVideoBufferDRMPRIME*>(picture.videoBuffer) != nullptr);
  if (m_isDRMPRIME)
  {
    const EGLDisplay eglDpy = GetDRMPRIMEEGLDisplay();
    for (auto& dt : m_drmTextures)
      dt.Init(eglDpy);
  }

  if (!m_plQueue)
  {
    m_plQueue = pl_queue_create(PL::PLInstance::Get()->m_plGpu);
    if (!m_plQueue)
      CLog::Log(LOGERROR, "CLinuxRendererPLBase::Configure - pl_queue_create failed");
  }
  m_queuePtsOffsetSet = false;

  // Cache display HDR capabilities for use in RenderHook (avoids per-frame allocation).
  if (this->m_passthroughHDR)
    m_displayHDRCaps = CServiceBroker::GetWinSystem()->GetDisplayHDRCapabilities();
  else
    m_displayHDRCaps = {};

  // Invalidate CMS state: source primaries may have changed and we are about
  // to render a new video source.
  m_plConfig->ResetCmsState();

  // Flush libplacebo's renderer caches (peak detection, frame mix state) so
  // the new source starts clean.  Required when switching content.
  if (const auto inst = PL::PLInstance::Get())
    pl_renderer_flush_cache(inst->GetRenderer());

  // Invalidate per-session render caches: the FBO or window size may have changed
  // and Kodi's VAO may have been recreated since the last Configure call.
  if (m_cachedFboTex)
  {
    pl_tex_destroy(PL::PLInstance::Get()->m_plGpu, &m_cachedFboTex);
    m_cachedFboTex = nullptr;
  }
  m_cachedFboId = UINT_MAX;
  m_cachedFboW = 0;
  m_cachedFboH = 0;
  m_kodiVaoCached = false;
  m_glStateCached = false;

  // Detect GL_KHR_no_error: if the context was created with no-error mode,
  // glGetError() is a no-op and we can skip the pre-render drain entirely.
  // GL_CONTEXT_FLAGS is a desktop GL query; GLES does not expose it.
#if defined(HAS_GL)
  {
    GLint ctxFlags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &ctxFlags);
    m_glNoError = (ctxFlags & GL_CONTEXT_FLAG_NO_ERROR_BIT_KHR) != 0;
  }
#else
  // GLES: GL_CONTEXT_FLAGS is not available. Query the EGL context attribute instead.
  // Mesa on RPi5 (and other platforms) sets EGL_CONTEXT_OPENGL_NO_ERROR_KHR when the
  // context is created with KHR_no_error, allowing us to skip the glGetError() drain.
  {
    EGLint noError = EGL_FALSE;
    if (m_eglDisplay != EGL_NO_DISPLAY)
      eglQueryContext(m_eglDisplay, eglGetCurrentContext(),
                      EGL_CONTEXT_OPENGL_NO_ERROR_KHR, &noError);
    m_glNoError = (noError == EGL_TRUE);
  }
#endif

  // Probe EGL_ANDROID_native_fence_sync + KHR_wait_sync
  // Used by both VAAPI (replaces vaSyncSurface() CPU stall) and DRMPRIME
  // (adds explicit GPU-side sync for V4L2/VC4 decode fences on RPi5 etc.).
  // Only meaningful when DMA_BUF_IOCTL_EXPORT_SYNC_FILE is also available
  // at compile time (kernel ≥ 5.2 headers).
  {
    auto* createSync = eglGetProcAddress("eglCreateSyncKHR");
    auto* destroySync = eglGetProcAddress("eglDestroySyncKHR");
    auto* waitSync = eglGetProcAddress("eglWaitSyncKHR");
    if (createSync && destroySync && waitSync)
    {
      m_eglCreateSyncKHR = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(createSync);
      m_eglDestroySyncKHR = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(destroySync);
      m_eglWaitSyncKHR = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(waitSync);
      m_hasEGLSyncFence = true;
    }
    else
    {
      m_hasEGLSyncFence = false;
    }
  }

  return true;
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::ConfigChanged(const VideoPicture& picture)
{
  return picture.videoBuffer->GetFormat() != m_format;
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::Flush(bool saveBuffers)
{
  if (m_plQueue)
  {
    pl_queue_reset(m_plQueue);
    m_queuePtsOffsetSet = false;
  }
  // The FBO may be recreated after a seek/stop; drop the cached wrapper so
  // RenderHook re-wraps with the current FBO on the next frame.
  if (m_cachedFboTex)
  {
    pl_tex_destroy(PL::PLInstance::Get()->m_plGpu, &m_cachedFboTex);
    m_cachedFboTex = nullptr;
  }
  m_cachedFboId = UINT_MAX;
  m_cachedFboW = 0;
  m_cachedFboH = 0;
  // VAO is stable across seeks; no need to invalidate m_kodiVaoCached.
  // Viewport/scissor state is window-level; invalidate conservatively.
  m_glStateCached = false;
  return TBase::Flush(saveBuffers);
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::AddVideoPicture(const VideoPicture& picture, int index)
{
  TBase::AddVideoPicture(picture, index);
  m_plBuffers[index].loaded = false;

  if (!m_plQueue || this->m_fps <= 0.0f)
    return;

  if (!m_queuePtsOffsetSet)
  {
    m_queuePtsOffset = picture.pts;
    m_queuePtsOffsetSet = true;
  }

  auto* qf = new QueuedFrameState{index, this};

  pl_source_frame src{};
  src.pts = picture.pts - m_queuePtsOffset;
  src.duration = 1.0 / static_cast<double>(this->m_fps);
  if (picture.iFlags & DVP_FLAG_INTERLACED)
    src.first_field = (picture.iFlags & DVP_FLAG_TOP_FIELD_FIRST) ? PL_FIELD_TOP : PL_FIELD_BOTTOM;
  src.frame_data = qf;
  src.map = &CLinuxRendererPLBase<TBase>::MapCallback;
  src.unmap = &CLinuxRendererPLBase<TBase>::UnmapCallback;
  src.discard = &CLinuxRendererPLBase<TBase>::DiscardCallback;

  pl_queue_push(m_plQueue, &src);
}

// ---------------------------------------------------------------------------
// pl_queue callbacks
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::MapCallback(pl_gpu /*gpu*/,
                                              pl_tex* /*tex*/,
                                              const struct pl_source_frame* src,
                                              struct pl_frame* out)
{
  // libplacebo does not zero the pl_frame before calling map(); initialize it
  // so that crop={0,0,0,0} (full texture), field=PL_FIELD_NONE, etc. are clean.
  *out = {};

  auto* qf = static_cast<QueuedFrameState*>(src->frame_data);
  CLinuxRendererPLBase<TBase>* r = qf->renderer;
  const int idx = qf->bufferIndex;

  if (!r->UploadTexture(idx))
    return false;

  const PLBuffer& plbuf = r->m_plBuffers[idx];
  if (!plbuf.loaded)
    return false;

  out->num_planes = plbuf.num_planes;
  for (int n = 0; n < plbuf.num_planes; ++n)
    out->planes[n] = plbuf.planes[n];
  out->color = plbuf.colorSpace;
  out->repr = plbuf.colorRepr;
  pl_frame_set_chroma_location(out, r->m_chromaLocation);
  out->rotation = PL::RotationFromOrientation(r->m_renderOrientation);

  // Set source crop so zoom/stretch/pixel ratio affect the source region.
  CRect srcRect, dstRect, viewRect;
  r->GetVideoRect(srcRect, dstRect, viewRect);
  out->crop = {srcRect.x1, srcRect.y1, srcRect.x2, srcRect.y2};

  // pl_queue sets out->field after map() returns based on first_field splitting.
  out->field = PL_FIELD_NONE;

  // Leave planes[n].flipped as set by UploadTexture (true for EGLImage DMA-buf
  // imports where DMA-buf row 0 = top of video but GL texcoord t=0 = bottom).
  // libplacebo's pass_align_planes inverts the sampling rect for flipped planes.

  return true;
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::UnmapCallback(pl_gpu /*gpu*/,
                                                struct pl_frame* /*frame*/,
                                                const struct pl_source_frame* src)
{
  // Textures remain in m_plBuffers and are freed by DeleteTexture / ReleasePLBuffer.
  delete static_cast<QueuedFrameState*>(src->frame_data);
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::DiscardCallback(const struct pl_source_frame* src)
{
  delete static_cast<QueuedFrameState*>(src->frame_data);
}

// ---------------------------------------------------------------------------
// Feature / scaling support
// ---------------------------------------------------------------------------

template<typename TBase>
CRenderInfo CLinuxRendererPLBase<TBase>::GetRenderInfo()
{
  CRenderInfo info = TBase::GetRenderInfo();
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_NONE);
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_AUTO);
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_LIBPLACEBO_BOB);
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_LIBPLACEBO_YADIF);
  info.m_deintMethods.push_back(VS_INTERLACEMETHOD_LIBPLACEBO_BWDIF);
  return info;
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::Supports(ERENDERFEATURE feature) const
{
  switch (feature)
  {
    case RENDERFEATURE_ZOOM:
    case RENDERFEATURE_VERTICAL_SHIFT:
    case RENDERFEATURE_PIXEL_RATIO:
    case RENDERFEATURE_STRETCH:
    case RENDERFEATURE_ROTATION:
    case RENDERFEATURE_TONEMAP:
    case RENDERFEATURE_BRIGHTNESS:
    case RENDERFEATURE_CONTRAST:
      return true;
    default:
      return false;
  }
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::Supports(ESCALINGMETHOD method) const
{
  switch (method)
  {
    case VS_SCALINGMETHOD_AUTO:
    case VS_SCALINGMETHOD_NEAREST:
    case VS_SCALINGMETHOD_LINEAR:
    case VS_SCALINGMETHOD_LANCZOS2:
    case VS_SCALINGMETHOD_LANCZOS3:
    case VS_SCALINGMETHOD_LANCZOS3_FAST:
    case VS_SCALINGMETHOD_SPLINE36:
    case VS_SCALINGMETHOD_SPLINE36_FAST:
    case VS_SCALINGMETHOD_CUBIC_MITCHELL:
    case VS_SCALINGMETHOD_CUBIC_CATMULL:
    case VS_SCALINGMETHOD_CUBIC_B_SPLINE:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// UpdateVideoFilter — apply Kodi GUI settings to libplacebo render params
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::UpdateVideoFilter()
{
  TBase::UpdateVideoFilter();
  m_plConfig->UpdateVideoFilter(this->m_scalingMethod, this->m_videoSettings);
}

template<typename TBase>
EShaderFormat CLinuxRendererPLBase<TBase>::GetShaderFormat()
{
  return SHADER_NONE;
}

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::LoadShadersHook()
{
  // Prevent TBase from loading its own YUV shaders; we render via RenderHook.
  // RENDER_GLSL is a file-scope enum value defined in both LinuxRendererGL.h and
  // LinuxRendererGLES.h, so it is visible here as a non-dependent name.
  this->m_renderMethod = RENDER_GLSL;
  return true;
}

// ---------------------------------------------------------------------------
// Texture lifecycle
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::CreateTexture(int index)
{
  auto& buf = this->m_buffers[index];
  auto& im = buf.image;
  memset(&im, 0, sizeof(im));
  im.height = this->m_sourceHeight;
  im.width = this->m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;

  // Generate a real dummy texture so ValidateRenderer passes safely.
  GLuint dummyTex = 0;
  glGenTextures(1, &dummyTex);
  buf.fields[0][0].id = dummyTex; // 0 == FIELD_FULL
  return true;
}

template<typename TBase>
void CLinuxRendererPLBase<TBase>::DeleteTexture(int index)
{
  ReleasePLBuffer(index);
  ReleaseSWBuffer(index);
  if (m_drmTexCache[index].tex)
  {
    pl_tex_destroy(PL::PLInstance::Get()->m_plGpu, &m_drmTexCache[index].tex);
    m_drmTexCache[index] = {};
  }
  GLuint dummyTex = this->m_buffers[index].fields[0][0].id; // 0 == FIELD_FULL
  if (dummyTex > 0)
    glDeleteTextures(1, &dummyTex);
  this->m_buffers[index].fields[0][0].id = 0;
}

// ---------------------------------------------------------------------------
// UploadTexture — VAAPI / DRMPRIME / software paths
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadTexture(int index)
{
  auto& buf = this->m_buffers[index];
  if (!buf.videoBuffer)
    return false;

  PLBuffer& plbuf = m_plBuffers[index];
  if (plbuf.loaded) [[likely]]
    return true;
  ReleasePLBuffer(index);

#if defined(HAVE_LIBVA)
  if (m_isVAAPI)
    return UploadVAAPI(index, plbuf);
#endif

  if (m_isDRMPRIME)
    return UploadDRMPRIME(index, plbuf);

  return UploadSoftware(index, plbuf);
}

// ---------------------------------------------------------------------------
// UploadVAAPI — VAAPI: export surface as DRM PRIME 2 → EGLImage → GL tex → pl_opengl_wrap
// ---------------------------------------------------------------------------

#if defined(HAVE_LIBVA)
template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadVAAPI(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];

  auto* vaaPic = dynamic_cast<VAAPI::CVaapiRenderPicture*>(buf.videoBuffer);
  if (!vaaPic)
    return false;

  if (!m_eglCreateImageKHR || !m_eglDestroyImageKHR || !m_glEGLImageTargetTexture2DOES)
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadVAAPI - EGL interop not available");
    return false;
  }

  VASurfaceID surface = vaaPic->procPic.videoSurface;
  if (surface == VA_INVALID_ID && vaaPic->avFrame)
    surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(vaaPic->avFrame->data[3]));
  if (surface == VA_INVALID_ID)
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadVAAPI - no valid VAAPI surface");
    return false;
  }

  VADisplay vadsp = vaaPic->vadsp;
  // Synchronize the VAAPI surface before we import its DMA-bufs.
  //
  // Priority order (best → worst):
  //  1. Implicit fencing (m_hasEGLModifiers): the kernel/EGL stack inserts a
  //     DMA-buf fence automatically when eglCreateImageKHR is called.  No
  //     explicit sync needed at all.
  //  2. EGL_ANDROID_native_fence_sync (m_hasEGLSyncFence): export a sync-file fd
  //     from the DMA-buf via DMA_BUF_IOCTL_EXPORT_SYNC_FILE, then import it as an
  //     EGL Sync Object.  eglWaitSyncKHR (called in RenderHook) inserts the
  //     wait into the GPU command stream — the CPU thread returns immediately.
  //  3. vaSyncSurface(): CPU-blocking stall.  Used only when neither of the
  //     above is available.
  if (!m_hasEGLModifiers)
  {
    bool syncHandled = false;
#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
    if (m_hasEGLSyncFence)
    {
      // vaExportSurfaceHandle (called below) gives us the DMA-buf fds.
      // We need to export the fence AFTER vaExportSurfaceHandle so the
      // descriptor is populated.  Defer to after the export call by setting
      // a flag; we'll do the ioctl there.
      syncHandled = true; // fence will be created after vaExportSurfaceHandle
    }
#endif
    if (!syncHandled)
      vaSyncSurface(vadsp, surface);
  }

  VADRMPRIMESurfaceDescriptor desc{};
  const VAStatus vaStatus =
      vaExportSurfaceHandle(vadsp, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
  if (vaStatus != VA_STATUS_SUCCESS)
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadVAAPI - vaExportSurfaceHandle failed: {}",
              vaErrorStr(vaStatus));
    return false;
  }

  plbuf.vaapiNumFds = static_cast<int>(
      std::min(desc.num_objects, static_cast<uint32_t>(std::size(plbuf.vaapiExportedFd))));
  for (int obj = 0; obj < plbuf.vaapiNumFds; ++obj)
    plbuf.vaapiExportedFd[obj] = desc.objects[obj].fd;

#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
  // EGL_ANDROID_native_fence_sync path: export a read fence from the first DMA-buf
  // object and import it as an EGL Sync object. eglWaitSyncKHR (called in
  // RenderHook) submits the wait to the GPU command stream asynchronously —
  // the CPU is not stalled here.
  if (!m_hasEGLModifiers && m_hasEGLSyncFence && plbuf.vaapiNumFds > 0)
  {
    dma_buf_export_sync_file syncExport{};
    syncExport.flags = DMA_BUF_SYNC_READ;
    syncExport.fd = -1;
    if (ioctl(plbuf.vaapiExportedFd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &syncExport) == 0 &&
        syncExport.fd >= 0)
    {
      const EGLint attribs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, syncExport.fd, EGL_NONE };
      plbuf.eglSyncFence = m_eglCreateSyncKHR(m_eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
      
      // If EGL failed to create the sync object, we must close the fd ourselves
      if (plbuf.eglSyncFence == EGL_NO_SYNC_KHR)
      {
        close(syncExport.fd);
      }
    }
    else
    {
      // ioctl not available at runtime (older kernel); fall back to CPU sync.
      vaSyncSurface(vadsp, surface);
    }
  }
#endif

  const auto plInst = PL::PLInstance::Get();
  const pl_gpu gpu = plInst->GetGpu();
  const GLenum vaapiTexTarget = GetVaapiTexTarget();
  bool success = true;
  const uint32_t numLayers = std::min(desc.num_layers, 3u);
  glGenTextures(static_cast<GLsizei>(numLayers), plbuf.vaapiGLTex);

  for (uint32_t i = 0; i < numLayers && success; ++i)
  {
    const auto& layer = desc.layers[i];
    const auto& object = desc.objects[layer.object_index[0]];

    const EGLint planeW =
        (i == 0) ? static_cast<EGLint>(desc.width) : (static_cast<EGLint>(desc.width) + 1) / 2;
    const EGLint planeH =
        (i == 0) ? static_cast<EGLint>(desc.height) : (static_cast<EGLint>(desc.height) + 1) / 2;

    // 6 mandatory attribute pairs + 2 optional modifier pairs + EGL_NONE terminator
    static constexpr int kEGLAttribsMax = 6 * 2 + 2 * 2 + 1;
    static_assert(kEGLAttribsMax == 17, "Update kEGLAttribsMax if adding more EGL attributes");
    EGLint attribs[kEGLAttribsMax];
    EGLint* a = attribs;
    *a++ = EGL_LINUX_DRM_FOURCC_EXT;
    *a++ = static_cast<EGLint>(layer.drm_format);
    *a++ = EGL_WIDTH;
    *a++ = planeW;
    *a++ = EGL_HEIGHT;
    *a++ = planeH;
    *a++ = EGL_DMA_BUF_PLANE0_FD_EXT;
    *a++ = object.fd;
    *a++ = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    *a++ = static_cast<EGLint>(layer.offset[0]);
    *a++ = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    *a++ = static_cast<EGLint>(layer.pitch[0]);
    if (m_hasEGLModifiers && object.drm_format_modifier != DRM_FORMAT_MOD_INVALID)
    {
      *a++ = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
      *a++ = static_cast<EGLint>(object.drm_format_modifier & 0xFFFFFFFFu);
      *a++ = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
      *a++ = static_cast<EGLint>(object.drm_format_modifier >> 32);
    }
    *a = EGL_NONE;

    EGLImageKHR eglImage =
        m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (!eglImage)
    {
      CLog::Log(LOGERROR,
                "CLinuxRendererPLBase::UploadVAAPI - eglCreateImageKHR failed for plane {} "
                "(EGL error 0x{:x})",
                i, static_cast<unsigned>(eglGetError()));
      success = false;
      break;
    }
    plbuf.vaapiEGLImage[i] = eglImage;

    glBindTexture(vaapiTexTarget, plbuf.vaapiGLTex[i]);
    glTexParameteri(vaapiTexTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(vaapiTexTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(vaapiTexTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(vaapiTexTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (m_hasEGLImageStorage)
      m_glEGLImageTargetTexStorageEXT(vaapiTexTarget, eglImage, nullptr);
    else
      m_glEGLImageTargetTexture2DOES(vaapiTexTarget, eglImage);

    GLenum glIformat = 0;
    switch (layer.drm_format)
    {
      case DRM_FORMAT_R8:
        glIformat = GL_R8;
        break;
      case DRM_FORMAT_GR88:
        glIformat = GL_RG8;
        break;
      case DRM_FORMAT_R16:
        glIformat = GL_R16;
        break;
      case DRM_FORMAT_GR1616:
        glIformat = GL_RG16;
        break;
      case DRM_FORMAT_ABGR8888:
      case DRM_FORMAT_XBGR8888:
        glIformat = GL_RGBA8;
        break;
#ifdef DRM_FORMAT_P030
      case DRM_FORMAT_P030:
        // 10-bit packed: 3x10-bit components in 32-bit word. Map as R16/RG16
        // so libplacebo sees the correct container width; actual bit depth is
        // conveyed via pl_bit_encoding.
        glIformat = (i == 0) ? GL_R16 : GL_RG16;
        break;
#endif
      default:
        CLog::Log(LOGERROR,
                  "CLinuxRendererPLBase::UploadVAAPI - unsupported DRM fourcc 0x{:x} for plane {}",
                  layer.drm_format, i);
        success = false;
        break;
    }
    if (!success)
      break;

    pl_opengl_wrap_params wp{};
    wp.texture = plbuf.vaapiGLTex[i];
    wp.target = vaapiTexTarget; // GL_TEXTURE_2D (desktop) or GL_TEXTURE_EXTERNAL_OES (GLES)
    wp.iformat = static_cast<int>(glIformat);
    wp.width = planeW;
    wp.height = planeH;

    plbuf.tex[i] = pl_opengl_wrap(gpu, &wp);
    if (!plbuf.tex[i])
    {
      CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadVAAPI - pl_opengl_wrap failed for plane {}",
                i);
      success = false;
    }
  }

  if (!success)
  {
    ReleasePLBuffer(index);
    return false;
  }

  plbuf.num_planes = static_cast<int>(numLayers);

  plbuf.planes[0] = {};
  plbuf.planes[0].texture = plbuf.tex[0];
  plbuf.planes[0].components = 1;
  plbuf.planes[0].component_mapping[0] = PL_CHANNEL_Y;
  plbuf.planes[0].component_mapping[1] = PL_CHANNEL_NONE;
  plbuf.planes[0].component_mapping[2] = PL_CHANNEL_NONE;
  plbuf.planes[0].component_mapping[3] = PL_CHANNEL_NONE;
  plbuf.planes[0].flipped = true;

  if (numLayers > 1)
  {
    plbuf.planes[1] = {};
    plbuf.planes[1].texture = plbuf.tex[1];
    plbuf.planes[1].components = 2;
    plbuf.planes[1].component_mapping[0] = PL_CHANNEL_CB;
    plbuf.planes[1].component_mapping[1] = PL_CHANNEL_CR;
    plbuf.planes[1].component_mapping[2] = PL_CHANNEL_NONE;
    plbuf.planes[1].component_mapping[3] = PL_CHANNEL_NONE;
    plbuf.planes[1].flipped = true;
  }

  plbuf.colorSpace = {};
  plbuf.colorRepr = {};
  plbuf.colorRepr.sys = pl_system_from_av(buf.m_srcColSpace);
  if (plbuf.colorRepr.sys == PL_COLOR_SYSTEM_UNKNOWN)
    plbuf.colorRepr.sys = pl_color_system_guess_ycbcr(this->m_sourceWidth, this->m_sourceHeight);
  plbuf.colorRepr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  plbuf.colorSpace.primaries = pl_primaries_from_av(buf.m_srcPrimaries);
  plbuf.colorSpace.transfer = pl_transfer_from_av(buf.m_srcColTransfer);

  switch (desc.fourcc)
  {
    case VA_FOURCC_P010:
      plbuf.colorRepr.bits = {16, 10, 6};
      break;
    case VA_FOURCC_P016:
      plbuf.colorRepr.bits = {16, 16, 0};
      break;
    default:
      break;
  }

  PL::ApplyHdrMetadata(plbuf.colorSpace, plbuf.colorRepr, plbuf.doviMetadata, buf);
  PL::ValidateInputColorSpace(plbuf.colorSpace, plbuf.colorRepr);

  plbuf.iFlags = buf.iFlags;
  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}
#endif // HAVE_LIBVA

// ---------------------------------------------------------------------------
// UploadDRMPRIME — DMA-buf → single combined OES texture → pl_tex
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadDRMPRIME(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];

  auto* drmBuf = dynamic_cast<CVideoBufferDRMPRIME*>(buf.videoBuffer);
  if (!drmBuf)
    return false;

  m_drmTextures[index].Unmap(); // defensive — no-op if not mapped
  if (!m_drmTextures[index].Map(drmBuf))
  {
    CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadDRMPRIME - CDRMPRIMETexture::Map failed");
    return false;
  }

  const pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  GLuint glTex = m_drmTextures[index].GetTexture();
  const CSizeInt sz = m_drmTextures[index].GetTextureSize();

  // Reuse cached pl_tex wrapper when the underlying GL texture and dimensions
  // haven't changed. pl_opengl_wrap + pl_tex_destroy cost a glGetError() stall
  // inside libplacebo (gl_check_err), plus mutex round-trips and a heap alloc/free.
  // The GL texture ID is stable for a given DRMPRIME slot; only the EGL image
  // behind it changes per frame, which pl_opengl_wrap doesn't interact with.
  auto& cache = m_drmTexCache[index];
  if (cache.tex && cache.glTex == glTex && cache.width == sz.Width() &&
      cache.height == sz.Height())
  {
    plbuf.tex[0] = cache.tex;
  }
  else
  {
    if (cache.tex)
      pl_tex_destroy(gpu, &cache.tex);

    pl_opengl_wrap_params wp{};
    wp.texture = glTex;
    wp.target = GL_TEXTURE_EXTERNAL_OES;
    // GL_TEXTURE_EXTERNAL_OES is opaque to GL — the EGL/DRM layer controls
    // the actual pixel format internally. libplacebo cannot map format-specific
    // ifomats (GL_RGB10_A2, GL_RGBA16F) for external OES targets. Pass GL_RGBA8
    // for all bit depths; actual precision is carried via color metadata.
    wp.iformat = GL_RGBA8;
    wp.width = sz.Width();
    wp.height = sz.Height();

    plbuf.tex[0] = pl_opengl_wrap(gpu, &wp);
    if (!plbuf.tex[0])
    {
      CLog::Log(LOGERROR, "CLinuxRendererPLBase::UploadDRMPRIME - pl_opengl_wrap failed");
      m_drmTextures[index].Unmap();
      return false;
    }

    cache.tex = plbuf.tex[0];
    cache.glTex = glTex;
    cache.width = sz.Width();
    cache.height = sz.Height();
  }

  plbuf.planes[0] = {};
  plbuf.planes[0].texture = plbuf.tex[0];
  plbuf.planes[0].components = 3;
  plbuf.planes[0].component_mapping[0] = PL_CHANNEL_R;
  plbuf.planes[0].component_mapping[1] = PL_CHANNEL_G;
  plbuf.planes[0].component_mapping[2] = PL_CHANNEL_B;
  plbuf.planes[0].component_mapping[3] = PL_CHANNEL_NONE;
  plbuf.planes[0].flipped = true; // DMA-buf row 0 = top of video, GL texcoord t=0 = bottom
  plbuf.num_planes = 1;

  // The GPU driver applies the YCbCr→RGB matrix when the OES texture is sampled,
  // yielding RGB in the source primaries/transfer. Tell libplacebo this is RGB.
  plbuf.colorSpace = {};
  plbuf.colorRepr = {};
  plbuf.colorRepr.sys = PL_COLOR_SYSTEM_RGB;
  plbuf.colorRepr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  plbuf.colorSpace.primaries = pl_primaries_from_av(buf.m_srcPrimaries);
  plbuf.colorSpace.transfer = pl_transfer_from_av(buf.m_srcColTransfer);

  PL::ApplyHdrMetadata(plbuf.colorSpace, plbuf.colorRepr, plbuf.doviMetadata, buf);

  // OES textures: the GPU driver already applied the YCbCr→RGB matrix when
  // sampling.  DV reshaping curves (colorRepr.dovi) operate on YCbCr input and
  // cannot be applied on post-conversion RGB data — doing so produces wrong
  // colours.  Keep the DV-informed HDR metadata (luminance from RPU) for tone
  // mapping, but force RGB system and clear the reshaping pointer.
  plbuf.colorRepr.sys = PL_COLOR_SYSTEM_RGB;
  plbuf.colorRepr.dovi = nullptr;

  PL::ValidateInputColorSpace(plbuf.colorSpace, plbuf.colorRepr);

  plbuf.iFlags = buf.iFlags;

#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
  // GPU-side fence for the V4L2 / DRMPRIME decode path (e.g. bcm2835-codec on
  // RPi5).  The VC4/V3D pipeline sets a DMA-buf read fence on the output
  // buffer when decode completes; we export it as a sync-file and import it
  // as an EGL Sync object.  eglWaitSyncKHR (called in RenderHook) inserts the
  // wait into the GPU command stream without stalling the CPU.
  //
  // Skipped when m_hasEGLModifiers is true: in that case eglCreateImageKHR
  // (called inside CDRMPRIMETexture::Map above) already attached the DMA-buf
  // reservation fence implicitly — adding an explicit explicit sync is redundant.
  if (m_hasEGLSyncFence && !m_hasEGLModifiers)
  {
    const AVDRMFrameDescriptor* desc = drmBuf->GetDescriptor();
    if (desc && desc->nb_objects > 0)
    {
      dma_buf_export_sync_file syncExport{};
      syncExport.flags = DMA_BUF_SYNC_READ;
      syncExport.fd = -1;
      if (ioctl(desc->objects[0].fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &syncExport) == 0 &&
          syncExport.fd >= 0)
      {
        EGLint attribs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, syncExport.fd, EGL_NONE };
        plbuf.eglSyncFence = m_eglCreateSyncKHR(m_eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
        
        if (plbuf.eglSyncFence == EGL_NO_SYNC_KHR)
        {
          close(syncExport.fd);
        }
      }
    }
  }
#endif

  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}

// ---------------------------------------------------------------------------
// ReleaseSWBuffer — frees the persistent PBOs and GL textures for one slot
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::ReleaseSWBuffer(int index)
{
  SWBuffer& sw = m_swBuffers[index];
  for (int n = 0; n < static_cast<int>(std::size(sw.tex)); ++n)
  {
    if (sw.pbo[n])
    {
      glDeleteBuffers(1, &sw.pbo[n]);
      sw.pbo[n] = 0;
    }
    if (sw.tex[n])
    {
      glDeleteTextures(1, &sw.tex[n]);
      sw.tex[n] = 0;
    }
    sw.texW[n] = 0;
    sw.texH[n] = 0;
    sw.texIformat[n] = 0;
  }
}

// ---------------------------------------------------------------------------
// PlaneDataToGLFormats — map pl_plane_data layout to GL format/type/iformat
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::PlaneDataToGLFormats(
    const pl_plane_data& pd, GLenum& iformat, GLenum& format, GLenum& type, int& bytesPerPixel)
{
  int numComp = 0;
  for (const int c : pd.component_size)
    if (c > 0)
      ++numComp;
  if (numComp == 0)
    return false;

  bytesPerPixel = static_cast<int>(pd.pixel_stride);
  const bool wide = (bytesPerPixel > numComp); // >1 byte per component: 16-bit container

  switch (numComp)
  {
    case 1:
      iformat = wide ? GL_R16 : GL_R8;
      format = GL_RED;
      type = wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
      return true;
    case 2:
      iformat = wide ? GL_RG16 : GL_RG8;
      format = GL_RG;
      type = wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
      return true;
    case 4:
      iformat = wide ? GL_RGBA16 : GL_RGBA8;
      format = GL_RGBA;
      type = wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// UploadSoftware — async CPU-to-GPU upload via PBO + pl_opengl_wrap
//
// Replaces synchronous pl_upload_plane with a PBO pipeline:
//   1. Orphan PBO each frame  ->  no CPU/GPU sync stall
//   2. memcpy frame data into the mapped PBO (CPU-side write)
//   3. glTexSubImage2D with PBO bound  ->  driver schedules async DMA
//   4. pl_opengl_wrap wraps the persistent GL texture for libplacebo
//
// The GL textures (m_swBuffers) survive ReleasePLBuffer; only the lightweight
// pl_opengl_wrap descriptor (plbuf.tex[n]) is recreated each frame.
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::UploadSoftware(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];
  SWBuffer& sw = m_swBuffers[index];

  uint8_t* src[3]{};
  int srcStrides[3]{};
  buf.videoBuffer->GetPlanes(src);
  buf.videoBuffer->GetStrides(srcStrides);

  pl_bit_encoding bits{};
  pl_plane_data pdata[4]{};
  const AVPixelFormat fmt = m_format;
  plbuf.num_planes = pl_plane_data_from_pixfmt(pdata, &bits, fmt);
  if (plbuf.num_planes <= 0)
  {
    CLog::Log(LOGERROR,
              "CLinuxRendererPLBase::UploadSoftware - unsupported pixel format {} (buf reports {})",
              static_cast<int>(fmt), static_cast<int>(buf.videoBuffer->GetFormat()));
    return false;
  }

  // Derive chroma plane dimensions. Do NOT use AV_CEIL_RSHIFT(a, b) with a
  // runtime b -- the macro is implementation-defined for signed integers when b
  // is not a compile-time constant. Use the explicit ceiling-shift formula.
  const AVPixFmtDescriptor* fmtDesc = av_pix_fmt_desc_get(fmt);
  const int chromaShiftW = fmtDesc ? static_cast<int>(fmtDesc->log2_chroma_w) : 1;
  const int chromaShiftH = fmtDesc ? static_cast<int>(fmtDesc->log2_chroma_h) : 1;

  pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;

  for (int n = 0; n < plbuf.num_planes; ++n)
  {
    const int planeW = (n > 0) ? (this->m_sourceWidth + (1 << chromaShiftW) - 1) >> chromaShiftW
                               : this->m_sourceWidth;
    const int planeH = (n > 0) ? (this->m_sourceHeight + (1 << chromaShiftH) - 1) >> chromaShiftH
                               : this->m_sourceHeight;

    GLenum iformat, glFormat, glType;
    int bytesPerPixel;
    if (!PlaneDataToGLFormats(pdata[n], iformat, glFormat, glType, bytesPerPixel))
    {
      CLog::Log(LOGERROR,
                "CLinuxRendererPLBase::UploadSoftware - no GL format mapping for plane {}", n);
      return false;
    }

    // Reallocate the GL texture only when dimensions or format change.
    if (sw.tex[n] == 0 || sw.texW[n] != planeW || sw.texH[n] != planeH ||
        sw.texIformat[n] != iformat)
    {
      // The underlying GL texture is being reallocated; the old pl_tex wrapper
      // would hold a stale texture ID, so invalidate it now.
      if (plbuf.tex[n])
      {
        pl_tex_destroy(gpu, &plbuf.tex[n]);
        plbuf.tex[n] = nullptr;
      }

      if (sw.tex[n] == 0)
        glGenTextures(1, &sw.tex[n]);
      glBindTexture(GL_TEXTURE_2D, sw.tex[n]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(iformat), planeW, planeH, 0, glFormat,
                   glType, nullptr);
      glBindTexture(GL_TEXTURE_2D, 0);

      sw.texW[n] = planeW;
      sw.texH[n] = planeH;
      sw.texIformat[n] = iformat;
    }

    if (sw.pbo[n] == 0)
      glGenBuffers(1, &sw.pbo[n]);

    const GLsizeiptr pboSize = static_cast<GLsizeiptr>(srcStrides[n]) * planeH;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sw.pbo[n]);

    // (Re)allocate the PBO storage when size changes. glBufferData with nullptr
    // orphans any previous allocation without a CPU stall.
    if (sw.pboSize[n] != pboSize)
    {
      glBufferData(GL_PIXEL_UNPACK_BUFFER, pboSize, nullptr, GL_STREAM_DRAW);
      sw.pboSize[n] = pboSize;
    }

    // GL_MAP_INVALIDATE_BUFFER_BIT orphans the current content, avoiding pipeline
    // stalls if the GPU is still reading from the previous upload. This replaces
    // the old glBufferData(nullptr) + glMapBuffer pattern and is GLES 3.0 compatible.
    void* pboPtr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, pboSize,
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (!pboPtr)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      CLog::Log(LOGERROR,
                "CLinuxRendererPLBase::UploadSoftware - glMapBufferRange failed for plane {}", n);
      return false;
    }

    std::memcpy(pboPtr, src[n], static_cast<size_t>(srcStrides[n]) * planeH);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

    // With a PBO bound, glTexSubImage2D reads from the PBO's GPU address space
    // (nullptr = offset 0 into the PBO) rather than CPU memory. The driver can
    // schedule this DMA transfer asynchronously.
    const GLint rowLengthPixels = srcStrides[n] / bytesPerPixel;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLengthPixels);
    glBindTexture(GL_TEXTURE_2D, sw.tex[n]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, planeW, planeH, glFormat, glType, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // Wrap the persistent GL texture for libplacebo. pl_opengl_wrap allocates
    // only a small descriptor struct -- no GL calls. pl_tex_destroy (called in
    // ReleasePLBuffer) releases the wrapper without deleting the GL texture
    // because libplacebo honours the "external owner" contract for wrapped textures.
    if (!plbuf.tex[n])
    {
      pl_opengl_wrap_params wp{};
      wp.texture = sw.tex[n];
      wp.target = GL_TEXTURE_2D;
      wp.iformat = static_cast<int>(iformat);
      wp.width = planeW;
      wp.height = planeH;
      plbuf.tex[n] = pl_opengl_wrap(gpu, &wp);
      if (!plbuf.tex[n])
      {
        CLog::Log(LOGERROR,
                  "CLinuxRendererPLBase::UploadSoftware - pl_opengl_wrap failed for plane {}", n);
        return false;
      }
    }

    // Build the pl_plane descriptor using pl_plane_data's component mapping.
    plbuf.planes[n] = {};
    plbuf.planes[n].texture = plbuf.tex[n];
    for (int c = 0; c < 4; ++c)
    {
      if (pdata[n].component_size[c] > 0)
        plbuf.planes[n].component_mapping[plbuf.planes[n].components++] =
            static_cast<pl_channel>(pdata[n].component_map[c]);
    }
    // OpenGL's coordinate system is bottom-up; top-down memory uploads are inverted.
    plbuf.planes[n].flipped = true;
  }

  plbuf.colorSpace = {};
  plbuf.colorRepr = {};
  plbuf.colorSpace.primaries = pl_primaries_from_av(buf.m_srcPrimaries);
  plbuf.colorSpace.transfer = pl_transfer_from_av(buf.m_srcColTransfer);
  plbuf.colorRepr.sys = pl_system_from_av(buf.m_srcColSpace);
  if (plbuf.colorRepr.sys == PL_COLOR_SYSTEM_UNKNOWN)
    plbuf.colorRepr.sys = pl_color_system_guess_ycbcr(this->m_sourceWidth, this->m_sourceHeight);
  plbuf.colorRepr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  plbuf.colorRepr.bits = bits;

  PL::ApplyHdrMetadata(plbuf.colorSpace, plbuf.colorRepr, plbuf.doviMetadata, buf);
  PL::ValidateInputColorSpace(plbuf.colorSpace, plbuf.colorRepr);

  plbuf.iFlags = buf.iFlags;
  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}

// ---------------------------------------------------------------------------
// Main render hook
// ---------------------------------------------------------------------------

template<typename TBase>
bool CLinuxRendererPLBase<TBase>::RenderHook(int idx)
{
  PLBuffer& plbuf = m_plBuffers[idx];
  if (!plbuf.loaded) [[unlikely]]
    return false;

  const auto plInst = PL::PLInstance::Get();
  pl_gpu gpu = plInst->GetGpu();
  pl_renderer renderer = plInst->GetRenderer();

  // Build input frame (used as fallback if the queue path doesn't fire)
  pl_frame frameIn{};
  frameIn.num_planes = plbuf.num_planes;
  for (int n = 0; n < plbuf.num_planes; ++n)
    frameIn.planes[n] = plbuf.planes[n];
  frameIn.color = plbuf.colorSpace;
  frameIn.repr = plbuf.colorRepr;
  pl_frame_set_chroma_location(&frameIn, m_chromaLocation);

  frameIn.rotation = PL::RotationFromOrientation(this->m_renderOrientation);
  frameIn.field = PL_FIELD_NONE;

  CRect src, dst, view;
  this->GetVideoRect(src, dst, view);

  // Source crop: tells libplacebo which region of the video texture to display.
  // Without this, zoom, pixel ratio, and stretch features have no effect on the
  // source side — the entire texture is always rendered into the destination rect.
  frameIn.crop = {src.x1, src.y1, src.x2, src.y2};
  int viewW = static_cast<int>(view.Width());
  int viewH = static_cast<int>(view.Height());
  if (viewW <= 0 || viewH <= 0)
    return false;

  GLint currentFbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &currentFbo);
  const auto fboId = static_cast<unsigned int>(currentFbo);

  // Re-wrap the framebuffer only when it changes.  pl_opengl_wrap/pl_tex_destroy
  // on every frame is expensive: some drivers flush pending GPU work at this point
  // and libplacebo discards cached per-target state, forcing a full re-initialization
  // on the next pl_render_image[_mix] call.
  if (!m_cachedFboTex || fboId != m_cachedFboId || viewW != m_cachedFboW || viewH != m_cachedFboH)
  {
    if (m_cachedFboTex)
      pl_tex_destroy(gpu, &m_cachedFboTex);

    // Query the actual internal format of the framebuffer color attachment so
    // libplacebo knows the real output precision. On HDR-capable compositors
    // (e.g. KDE Wayland with HDR) the surface may be GL_RGB10_A2 or wider;
    // lying to libplacebo with GL_RGBA8 would cause it to dither/clamp
    // unnecessarily before the compositor receives the frame.
    GLenum attachment = (fboId == 0) ? GL_BACK : GL_COLOR_ATTACHMENT0;
    GLint redBits = 8;
    GLint alphaBits = 8;
    GLint colorType = GL_UNSIGNED_NORMALIZED;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &redBits);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alphaBits);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE,
                                          &colorType);
    GLenum fboIformat;
    if (colorType == GL_FLOAT)
      fboIformat = GL_RGBA16F;
    else if (redBits > 8 && alphaBits <= 2)
      fboIformat = GL_RGB10_A2;
    else if (redBits > 8)
      fboIformat = GL_RGBA16;
    else
      fboIformat = GL_RGBA8;

    pl_opengl_wrap_params wrapParams{};
    wrapParams.framebuffer = fboId;
    wrapParams.width = viewW;
    wrapParams.height = viewH;
    wrapParams.iformat = fboIformat;

    m_cachedFboTex = pl_opengl_wrap(gpu, &wrapParams);
    if (!m_cachedFboTex)
    {
      CLog::Log(LOGERROR, "CLinuxRendererPLBase::RenderHook - failed to wrap GL framebuffer");
      m_cachedFboId = UINT_MAX;
      return false;
    }
    m_cachedFboId = fboId;
    m_cachedFboW = viewW;
    m_cachedFboH = viewH;
    // FBO change implies the window/surface changed; invalidate cached GL state.
    m_glStateCached = false;
  }
  pl_tex outTex = m_cachedFboTex;

  pl_frame frameOut{};
  frameOut.num_planes = 1;
  frameOut.planes[0].texture = outTex;
  frameOut.planes[0].components = 3;
  frameOut.planes[0].component_mapping[0] = PL_CHANNEL_R;
  frameOut.planes[0].component_mapping[1] = PL_CHANNEL_G;
  frameOut.planes[0].component_mapping[2] = PL_CHANNEL_B;
  frameOut.planes[0].component_mapping[3] = PL_CHANNEL_NONE;
  frameOut.crop = {dst.x1, dst.y1, dst.x2, dst.y2};

  if (this->m_passthroughHDR)
  {
    const auto& buf = this->m_buffers[idx];
    frameOut.color.primaries = PL_COLOR_PRIM_BT_2020;
    frameOut.color.transfer =
        (buf.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67) ? PL_COLOR_TRC_HLG : PL_COLOR_TRC_PQ;

    // Set the display's actual peak luminance from EDID so libplacebo knows
    // the output capability. Without this, max_luma=0 causes libplacebo to
    // infer 203 nits (PL_COLOR_SDR_WHITE) via pl_color_transfer_nominal_peak
    // and tone-map all HDR content down to SDR levels.
    if (m_displayHDRCaps.GetDisplayMaxLuminance() > 0.0f)
    {
      frameOut.color.hdr.max_luma = m_displayHDRCaps.GetDisplayMaxLuminance();
      if (m_displayHDRCaps.GetDisplayMinLuminance() > 0.0f)
        frameOut.color.hdr.min_luma = m_displayHDRCaps.GetDisplayMinLuminance();
    }
  }
  else
  {
    frameOut.color.primaries = PL_COLOR_PRIM_BT_709;
    frameOut.color.transfer = PL_COLOR_TRC_BT_1886;

    // SDR display peak luminance for tone-mapping HDR→SDR output.
    const float peakLuminance = CServiceBroker::GetWinSystem()->GetGuiSdrPeakLuminance();
    if (peakLuminance > 0.0f)
      frameOut.color.hdr.max_luma = peakLuminance;
  }
  // Jointly infer source and destination color spaces. This is the canonical
  // libplacebo approach (matches mpv's vo_gpu_next): it infers src first, then
  // dst using src as reference, and coordinates SDR contrast (min_luma) between
  // them. For HLG→HDR, it also tunes the HLG source peak to the display peak.
  pl_color_space_infer_map(&frameIn.color, &frameOut.color);

  frameOut.repr.sys = PL_COLOR_SYSTEM_RGB;
  frameOut.repr.levels = CServiceBroker::GetWinSystem()->UseLimitedColor() ? PL_COLOR_LEVELS_LIMITED
                                                                           : PL_COLOR_LEVELS_FULL;

  // CMS (ICC profile / 3D LUT) is SDR colour management.  Do not apply it when
  // the output frame is in HDR passthrough mode (PQ/HLG transfer) — libplacebo
  // would apply SDR colour management on top of an HDR signal, producing wrong
  // colours (e.g. blown-out cyan sky when an SDR ICC profile is active).
  if (!this->m_passthroughHDR)
    m_plConfig->ApplyCMS(frameOut, this->m_srcPrimaries);

  pl_render_params params = m_plConfig->GetOptions()->params;
  params.border = PL_CLEAR_SKIP;

  // Don't cache single-display frames in GPU memory — most frames are shown once
  // and immediately replaced. Caching wastes VRAM and upload bandwidth.
  params.skip_caching_single_frame = true;

  // Keep frame mixing cache across renders when interpolation is active.
  // Disable interpolation entirely for still images (no adjacent frames to mix).
  if (params.frame_mixer)
    params.preserve_mixing_cache = true;

  // Allow peak detection to lag one frame — avoids a GPU pipeline stall
  // waiting for the compute shader result on the same frame it was measured.
  if (params.peak_detect_params)
  {
    static pl_peak_detect_params peakParams = pl_peak_detect_default_params;
    peakParams.allow_delayed = true;
    params.peak_detect_params = &peakParams;
  }

  // Dolby Vision RPU provides explicit per-frame luminance bounds in its metadata.
  // Libplacebo's peak detection pass is a redundant GPU sampling operation when DoVi
  // data is present — disable it to save a full GPU pass on every DV frame.
  if (plbuf.colorRepr.dovi != nullptr)
    params.peak_detect_params = nullptr;

  // Drain any pre-existing GL errors so libplacebo's gl_check_err doesn't abort
  // a pass early.  Skipped when the context was created with GL_KHR_no_error
  // (EGL_CONTEXT_OPENGL_NO_ERROR_KHR) since no errors can ever be generated.
  if (!m_glNoError)
  {
    while (glGetError() != GL_NO_ERROR)
      ;
  }

  // Save GL state that libplacebo modifies but may not restore on error paths.
  // Viewport, scissor enable, and scissor box are CPU-side shadow state in every
  // known GL/GLES driver (no GPU round-trip), but caching them still removes
  // 3 function calls from the hot path.  The cache is invalidated whenever the
  // FBO dimensions change (window resize) or on Configure/Flush.
  if (!m_glStateCached)
  {
    glGetIntegerv(GL_VIEWPORT, m_cachedGLViewport);
    m_cachedScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, m_cachedGLScissor);
    m_glStateCached = true;
  }
  const GLint* savedViewport = m_cachedGLViewport;
  const GLboolean scissorWasEnabled = m_cachedScissorEnabled;
  const GLint* savedScissor = m_cachedGLScissor;
  // libplacebo binds its own VAO per-pass and resets to VAO 0 at exit.
  // In GL/GLES core profile VAO 0 is invalid; restore Kodi's VAO.
  // Cache the result: glGetIntegerv(GL_VERTEX_ARRAY_BINDING) serializes the GPU
  // command stream on tile-based GPUs (Mali/Adreno/PowerVR) because the driver
  // must flush in-flight work before reading the register.  Kodi's VAO is created
  // once at startup and never changes, so we query it only on the first render.
  if (!m_kodiVaoCached)
  {
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_kodiVAO);
    m_kodiVaoCached = true;
  }
  const GLint savedVAO = m_kodiVAO;

  // GPU-side DMA-buf fence wait (EGL_ANDROID_native_fence_sync).
  // UploadTexture() may have imported a DMA-buf sync-file fence for VAAPI or
  // DRMPRIME frames.  Drain all pending sync objects here — after pl_queue_update
  // has completed its MapCallback chain, before any pl_render_image[_mix]
  // command touches the textures.  eglWaitSyncKHR inserts the wait into the
  // GPU command stream; the CPU thread returns immediately.
  if (m_hasEGLSyncFence)
  {
    for (auto& fbuf : m_plBuffers)
    {
      if (fbuf.eglSyncFence != EGL_NO_SYNC_KHR)
      {
        m_eglWaitSyncKHR(m_eglDisplay, fbuf.eglSyncFence, 0);
        m_eglDestroySyncKHR(m_eglDisplay, fbuf.eglSyncFence);
        fbuf.eglSyncFence = EGL_NO_SYNC_KHR;
      }
    }
  }

  // Queue-based render path (provides prev/curr/next frames for BWDIF/YADIF)
  bool rendered = false;
  if (m_plQueue && m_queuePtsOffsetSet)
  {
    const auto& buf = this->m_buffers[idx];
    const float vsyncDuration = 1.0f / CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();

    pl_queue_params qparams{};
    qparams.pts = buf.pts - m_queuePtsOffset;
    qparams.radius = pl_frame_mix_radius(&params);
    qparams.vsync_duration = vsyncDuration;
    // Allow up to 40% of a vsync period for the decoder to deliver the frame.
    // V4L2/bcm2835-codec on RPi5 has measurable decode jitter; a zero timeout causes
    // pl_queue_update to return PL_QUEUE_MORE on any slip, falling back to the direct
    // pl_render_image path and losing frame-mixing / motion-compensation.
    qparams.timeout = static_cast<uint64_t>(vsyncDuration * 0.4f * 1e9f);

    pl_frame_mix mix{};
    const auto status = pl_queue_update(m_plQueue, &mix, &qparams);
    if (status == PL_QUEUE_OK || status == PL_QUEUE_MORE) [[likely]]
    {
      if (!pl_render_image_mix(renderer, &mix, &frameOut, &params))
        CLog::Log(LOGWARNING, "CLinuxRendererPLBase::RenderHook - pl_render_image_mix failed");
      rendered = true;
    }
    else if (status == PL_QUEUE_ERR) [[unlikely]]
    {
      CLog::Log(LOGWARNING, "CLinuxRendererPLBase::RenderHook - pl_queue_update error");
    }
  }
  if (!rendered)
  {
    if (!pl_render_image(renderer, &frameIn, &frameOut, &params))
      CLog::Log(LOGWARNING, "CLinuxRendererPLBase::RenderHook - pl_render_image failed");
  }

  // Restore framebuffer: gl_tex_blit resets both bindings to 0 after every blit.
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, currentFbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

  glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
  if (scissorWasEnabled)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  glScissor(savedScissor[0], savedScissor[1], savedScissor[2], savedScissor[3]);

  glBindVertexArray(savedVAO);

  // Reset active texture unit (libplacebo may leave it at a non-zero index).
  glActiveTexture(GL_TEXTURE0);

  // Reset shader program: Kodi's CGLShader may skip glUseProgram if it thinks
  // its program is still bound; if libplacebo's program is active instead the
  // GUI renders with the wrong shader.
  glUseProgram(0);

  // outTex aliases m_cachedFboTex — do NOT destroy it here.  It lives across
  // frames and is released in Flush(), Configure(), and the destructor.

  // Always return true: we own this slot; the base must not fall through to
  // its own render path which would crash on uninitialized GL planes.
  return true;
}

// ---------------------------------------------------------------------------
// ReleasePLBuffer
// ---------------------------------------------------------------------------

template<typename TBase>
void CLinuxRendererPLBase<TBase>::ReleasePLBuffer(int index)
{
  PLBuffer& plbuf = m_plBuffers[index];

#if defined(HAVE_LIBVA)
  if (m_isVAAPI)
  {
    // Drop any pending EGL sync object. eglDestroySyncKHR deletes the object
    // immediately; if a wait is pending, the driver keeps the underlying fence
    // alive until the wait completes.
    if (plbuf.eglSyncFence != EGL_NO_SYNC_KHR && m_eglDestroySyncKHR)
    {
      m_eglDestroySyncKHR(m_eglDisplay, plbuf.eglSyncFence);
      plbuf.eglSyncFence = EGL_NO_SYNC_KHR;
    }

    pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
    for (int n = 0; n < static_cast<int>(std::size(plbuf.vaapiGLTex)); ++n)
    {
      if (plbuf.tex[n])
      {
        pl_tex_destroy(gpu, &plbuf.tex[n]);
        plbuf.tex[n] = nullptr;
      }
      if (plbuf.vaapiGLTex[n])
      {
        glDeleteTextures(1, &plbuf.vaapiGLTex[n]);
        plbuf.vaapiGLTex[n] = 0;
      }
      if (plbuf.vaapiEGLImage[n] != EGL_NO_IMAGE_KHR)
      {
        m_eglDestroyImageKHR(m_eglDisplay, plbuf.vaapiEGLImage[n]);
        plbuf.vaapiEGLImage[n] = EGL_NO_IMAGE_KHR;
      }
      plbuf.planes[n] = {};
    }
    for (int f = 0; f < plbuf.vaapiNumFds; ++f)
    {
      if (plbuf.vaapiExportedFd[f] >= 0)
      {
        close(plbuf.vaapiExportedFd[f]);
        plbuf.vaapiExportedFd[f] = -1;
      }
    }
    plbuf.vaapiNumFds = 0;
    plbuf.num_planes = 0;
    plbuf.loaded = false;
    return;
  }
#endif

  // Drop any pending EGL sync object (DRMPRIME path).
  if (plbuf.eglSyncFence != EGL_NO_SYNC_KHR && m_eglDestroySyncKHR)
  {
    m_eglDestroySyncKHR(m_eglDisplay, plbuf.eglSyncFence);
    plbuf.eglSyncFence = EGL_NO_SYNC_KHR;
  }

  m_drmTextures[index].Unmap();

  // For DRMPRIME, the pl_tex wrapper is owned by m_drmTexCache and reused across
  // frames (the underlying GL texture ID is stable). Don't destroy it here — just
  // null the slot so the render pipeline won't reference a stale frame.
  // For software-decode textures (no cache), destroy normally.
  pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  for (int n = 0; n < plbuf.num_planes; ++n)
  {
    if (plbuf.tex[n])
    {
      if (m_isDRMPRIME && m_drmTexCache[index].tex == plbuf.tex[n])
        plbuf.tex[n] = nullptr; // cache owns it
      else
        pl_tex_destroy(gpu, &plbuf.tex[n]);
    }
    plbuf.planes[n] = {};
  }
  plbuf.colorSpace = {};
  plbuf.colorRepr = {};
  plbuf.num_planes = 0;
  plbuf.loaded = false;
}
