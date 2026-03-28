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

#include "PlHelper.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/DRMPRIMEEGL.h"
#include "cores/VideoPlayer/VideoRenderers/VideoShaders/ShaderFormats.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include "system_gl.h"

#if defined(HAVE_LIBVA)
#include "cores/VideoPlayer/DVDCodecs/Video/VAAPI.h"

#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <va/va_drmcommon.h>

// Enum constants for GL_EXT_semaphore / GL_EXT_semaphore_fd.
// Defined here in case the system GL headers predate these extensions.
#ifndef GL_LAYOUT_GENERAL_EXT
#define GL_LAYOUT_GENERAL_EXT 0x958D
#endif
#ifndef GL_HANDLE_TYPE_SYNC_FD_EXT
#define GL_HANDLE_TYPE_SYNC_FD_EXT 0x9586
#endif
#endif // HAVE_LIBVA

extern "C"
{
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <array>

#include <libplacebo/utils/libav.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

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
class CRendererPLBase : public TBase
{
public:
  CRendererPLBase();
  ~CRendererPLBase() override;

  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation) override;
  bool ConfigChanged(const VideoPicture& picture) override;
  [[nodiscard]] bool Supports(ERENDERFEATURE feature) const override;
  [[nodiscard]] bool Supports(ESCALINGMETHOD method) const override;
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
  struct PLBuffer
  {
    pl_plane planes[3]{};
    pl_tex tex[3]{};
    int num_planes{0};
    pl_color_space colorSpace{};
    pl_color_repr colorRepr{};
    pl_dovi_metadata doviMetadata{}; ///< Owned copy; colorRepr.dovi points here when valid
    unsigned int iFlags{0}; ///< DVP_FLAG_* interlace flags
    bool loaded{false};
    // Pending GL semaphore imported from a DMA-buf sync-file fence
    // (GL_EXT_semaphore_fd).  Non-zero means glWaitSemaphoreEXT has NOT been
    // called yet for this slot.  Used by both the VAAPI and DRMPRIME paths.
    GLuint fenceSemaphore{0};
    GLuint fenceTextures[3]{}; ///< GL texture handles for the barrier
    int nFenceTextures{0};
#if defined(HAVE_LIBVA)
    GLuint vaapiGLTex[3]{};
    EGLImageKHR vaapiEGLImage[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    int vaapiExportedFd[4]{-1, -1, -1, -1};
    int vaapiNumFds{0};
#endif
  };

  std::array<PLBuffer, NUM_BUFFERS> m_plBuffers{};
  std::array<CDRMPRIMETexture, NUM_BUFFERS> m_drmTextures{};
  bool m_isDRMPRIME{false};
  // True when eglQueryDmaBufModifiersEXT is available: the EGL stack inserts
  // implicit DMA-buf fences at eglCreateImageKHR time, making explicit CPU or
  // GPU-side sync redundant for both VAAPI and DRMPRIME paths.
  bool m_hasEGLModifiers{false};

#if defined(HAVE_LIBVA)
  bool m_isVAAPI{false};
  EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};
  PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR{nullptr};
  PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR{nullptr};
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES{nullptr};
#endif

  AVPixelFormat m_format{AV_PIX_FMT_NONE};
  pl_color_space m_colorSpace{};
  pl_chroma_location m_chromaLocation{PL_CHROMA_UNKNOWN};
  pl_options m_plOpts{nullptr}; ///< Owns all libplacebo render parameters

  struct QueuedFrameState
  {
    int bufferIndex;
    CRendererPLBase* renderer;
  };
  // libplacebo guarantees exactly one of unmap/discard is called per pushed frame.
  // QueuedFrameState must be trivially destructible so raw delete is safe and
  // no destructor side-effects are silently skipped on the discard path.
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
  // glGetIntegerv(GL_VERTEX_ARRAY_BINDING) serialises the GPU command stream on
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

  // GL_EXT_semaphore + GL_EXT_semaphore_fd: GPU-side DMA-buf fence wait.
  //
  // Used for both VAAPI and DRMPRIME paths.  When these extensions and the
  // DMA_BUF_IOCTL_EXPORT_SYNC_FILE ioctl (kernel ≥ 5.2) are available, we
  // export the DMA-buf read fence as a sync-file fd, import it as a GL
  // semaphore, and call glWaitSemaphoreEXT — a GPU command-stream wait that
  // never stalls the CPU.
  //
  // Function pointer types use private aliases to avoid conflicts with system
  // GL headers that may define the same PFNGL…PROC typedefs differently.
  using FnGlGenSemaphoresEXT = void (*)(GLsizei, GLuint*);
  using FnGlDeleteSemaphoresEXT = void (*)(GLsizei, const GLuint*);
  using FnGlImportSemaphoreFdEXT = void (*)(GLuint, GLenum, GLint);
  using FnGlWaitSemaphoreEXT =
      void (*)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);

  FnGlGenSemaphoresEXT m_glGenSemaphoresEXT{nullptr};
  FnGlDeleteSemaphoresEXT m_glDeleteSemaphoresEXT{nullptr};
  FnGlImportSemaphoreFdEXT m_glImportSemaphoreFdEXT{nullptr};
  FnGlWaitSemaphoreEXT m_glWaitSemaphoreEXT{nullptr};
  bool m_hasGLSemaphoreFd{false};

  static void ApplyHdrMetadata(PLBuffer& pb, const auto& b);

  bool UploadVAAPI(int index, PLBuffer& plbuf);
  bool UploadDRMPRIME(int index, PLBuffer& plbuf);
  bool UploadSoftware(int index, PLBuffer& plbuf);

  static constexpr pl_rotation RotationFromOrientation(unsigned int deg)
  {
    switch (deg)
    {
      case 90:
        return PL_ROTATION_270;
      case 180:
        return PL_ROTATION_180;
      case 270:
        return PL_ROTATION_90;
      default:
        return PL_ROTATION_0;
    }
  }

  static bool MapCallback(pl_gpu gpu,
                          pl_tex* tex,
                          const struct pl_source_frame* src,
                          struct pl_frame* out);
  static void UnmapCallback(pl_gpu gpu, struct pl_frame* frame, const struct pl_source_frame* src);
  static void DiscardCallback(const struct pl_source_frame* src);

  void ReleasePLBuffer(int index);
};

// =============================================================================
// Template implementations
// =============================================================================

template<typename TBase>
CRendererPLBase<TBase>::CRendererPLBase()
{
  m_plOpts = pl_options_alloc(PL::PLInstance::Get()->m_plLog);
}

template<typename TBase>
CRendererPLBase<TBase>::~CRendererPLBase()
{
  for (int i = 0; i < NUM_BUFFERS; ++i)
    ReleasePLBuffer(i);
  if (m_plQueue)
  {
    pl_queue_destroy(&m_plQueue);
    m_plQueue = nullptr;
  }
  if (m_cachedFboTex)
  {
    pl_tex_destroy(PL::PLInstance::Get()->m_plGpu, &m_cachedFboTex);
    m_cachedFboTex = nullptr;
  }
  pl_options_free(&m_plOpts);
  PL::PLInstance::Get()->Reset();
}

// ---------------------------------------------------------------------------
// Configure / Flush / AddVideoPicture
// ---------------------------------------------------------------------------

template<typename TBase>
bool CRendererPLBase<TBase>::Configure(const VideoPicture& picture,
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

  // Probe EGL DMA-buf modifier support unconditionally: applies to both VAAPI
  // and DRMPRIME.  When present, eglCreateImageKHR implicitly attaches the
  // DMA-buf reservation fence to the EGLImage so the GPU waits automatically —
  // no explicit CPU stall or GL semaphore is needed.
  m_hasEGLModifiers = (eglGetProcAddress("eglQueryDmaBufModifiersEXT") != nullptr);

#if defined(HAVE_LIBVA)
  m_isVAAPI = (dynamic_cast<VAAPI::CVaapiRenderPicture*>(picture.videoBuffer) != nullptr);
  if (m_isVAAPI)
  {
    m_eglDisplay = eglGetCurrentDisplay();
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
    EGLDisplay eglDpy = GetDRMPRIMEEGLDisplay();
    for (auto& dt : m_drmTextures)
      dt.Init(eglDpy);
  }

  if (!m_plQueue)
  {
    m_plQueue = pl_queue_create(PL::PLInstance::Get()->m_plGpu);
    if (!m_plQueue)
      CLog::Log(LOGERROR, "CRendererPLBase::Configure - pl_queue_create failed");
  }
  m_queuePtsOffsetSet = false;

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
  {
    GLint ctxFlags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &ctxFlags);
    m_glNoError = (ctxFlags & GL_CONTEXT_FLAG_NO_ERROR_BIT_KHR) != 0;
  }

  // Probe GL_EXT_semaphore + GL_EXT_semaphore_fd.
  // Used by both VAAPI (replaces vaSyncSurface() CPU stall) and DRMPRIME
  // (adds explicit GPU-side sync for V4L2/VC4 decode fences on RPi5 etc.).
  // Only meaningful when DMA_BUF_IOCTL_EXPORT_SYNC_FILE is also available
  // at compile time (kernel ≥ 5.2 headers); the runtime ioctl call provides
  // its own fallback to vaSyncSurface() / no-sync if the ioctl fails.
  {
    auto* gen = eglGetProcAddress("glGenSemaphoresEXT");
    auto* del = eglGetProcAddress("glDeleteSemaphoresEXT");
    auto* imp = eglGetProcAddress("glImportSemaphoreFdEXT");
    auto* wai = eglGetProcAddress("glWaitSemaphoreEXT");
    if (gen && del && imp && wai)
    {
      m_glGenSemaphoresEXT = reinterpret_cast<FnGlGenSemaphoresEXT>(gen);
      m_glDeleteSemaphoresEXT = reinterpret_cast<FnGlDeleteSemaphoresEXT>(del);
      m_glImportSemaphoreFdEXT = reinterpret_cast<FnGlImportSemaphoreFdEXT>(imp);
      m_glWaitSemaphoreEXT = reinterpret_cast<FnGlWaitSemaphoreEXT>(wai);
      m_hasGLSemaphoreFd = true;
    }
    else
    {
      m_hasGLSemaphoreFd = false;
    }
  }

  return true;
}

template<typename TBase>
bool CRendererPLBase<TBase>::ConfigChanged(const VideoPicture& picture)
{
  return picture.videoBuffer->GetFormat() != m_format;
}

template<typename TBase>
bool CRendererPLBase<TBase>::Flush(bool saveBuffers)
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
void CRendererPLBase<TBase>::AddVideoPicture(const VideoPicture& picture, int index)
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
  src.map = &CRendererPLBase<TBase>::MapCallback;
  src.unmap = &CRendererPLBase<TBase>::UnmapCallback;
  src.discard = &CRendererPLBase<TBase>::DiscardCallback;

  pl_queue_push(m_plQueue, &src);
}

// ---------------------------------------------------------------------------
// pl_queue callbacks
// ---------------------------------------------------------------------------

template<typename TBase>
bool CRendererPLBase<TBase>::MapCallback(pl_gpu /*gpu*/,
                                         pl_tex* /*tex*/,
                                         const struct pl_source_frame* src,
                                         struct pl_frame* out)
{
  // libplacebo does not zero the pl_frame before calling map(); initialize it
  // so that crop={0,0,0,0} (full texture), field=PL_FIELD_NONE, etc. are clean.
  *out = {};

  auto* qf = static_cast<QueuedFrameState*>(src->frame_data);
  CRendererPLBase<TBase>* r = qf->renderer;
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

  out->rotation = CRendererPLBase<TBase>::RotationFromOrientation(r->m_renderOrientation);

  // pl_queue sets out->field after map() returns based on first_field splitting.
  out->field = PL_FIELD_NONE;

  // Leave planes[n].flipped as set by UploadTexture (true for EGLImage DMA-buf
  // imports where DMA-buf row 0 = top of video but GL texcoord t=0 = bottom).
  // libplacebo's pass_align_planes inverts the sampling rect for flipped planes.

  return true;
}

template<typename TBase>
void CRendererPLBase<TBase>::UnmapCallback(pl_gpu /*gpu*/,
                                           struct pl_frame* /*frame*/,
                                           const struct pl_source_frame* src)
{
  // Textures remain in m_plBuffers and are freed by DeleteTexture / ReleasePLBuffer.
  delete static_cast<QueuedFrameState*>(src->frame_data);
}

template<typename TBase>
void CRendererPLBase<TBase>::DiscardCallback(const struct pl_source_frame* src)
{
  delete static_cast<QueuedFrameState*>(src->frame_data);
}

// ---------------------------------------------------------------------------
// Feature / scaling support
// ---------------------------------------------------------------------------

template<typename TBase>
bool CRendererPLBase<TBase>::Supports(ERENDERFEATURE feature) const
{
  switch (feature)
  {
    case RENDERFEATURE_ZOOM:
    case RENDERFEATURE_VERTICAL_SHIFT:
    case RENDERFEATURE_PIXEL_RATIO:
    case RENDERFEATURE_STRETCH:
    case RENDERFEATURE_ROTATION:
    case RENDERFEATURE_TONEMAP:
      return true;
    default:
      return false;
  }
}

template<typename TBase>
bool CRendererPLBase<TBase>::Supports(ESCALINGMETHOD method) const
{
  switch (method)
  {
    case VS_SCALINGMETHOD_AUTO:
    case VS_SCALINGMETHOD_LINEAR:
    case VS_SCALINGMETHOD_LANCZOS3:
    case VS_SCALINGMETHOD_LANCZOS3_FAST:
    case VS_SCALINGMETHOD_SPLINE36:
    case VS_SCALINGMETHOD_SPLINE36_FAST:
    case VS_SCALINGMETHOD_CUBIC_MITCHELL:
    case VS_SCALINGMETHOD_CUBIC_CATMULL:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// UpdateVideoFilter — apply advancedsettings.xml to libplacebo render params
// ---------------------------------------------------------------------------

template<typename TBase>
void CRendererPLBase<TBase>::UpdateVideoFilter()
{
  TBase::UpdateVideoFilter();
  pl_options_reset(m_plOpts, nullptr);

  const auto& adv = *CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();
  auto applyStr = [&](const char* key, const std::string& val)
  {
    if (!val.empty())
      pl_options_set_str(m_plOpts, key, val.c_str());
  };

  // Scaling
  applyStr("preset", adv.m_libplaceboPreset);
  applyStr("upscaler", adv.m_libplaceboUpscaler);
  applyStr("downscaler", adv.m_libplaceboDownscaler);
  applyStr("frame_mixer", adv.m_libplaceboFrameMixer);
  applyStr("antiringing_strength", adv.m_libplaceboAntiringing);
  applyStr("sigmoid", adv.m_libplaceboSigmoid);

  // Debanding
  applyStr("deband", adv.m_libplaceboDeband);
  applyStr("deband_iterations", adv.m_libplaceboDebandIterations);
  applyStr("deband_threshold", adv.m_libplaceboDebandThreshold);
  applyStr("deband_radius", adv.m_libplaceboDebandRadius);
  applyStr("deband_grain", adv.m_libplaceboDebandGrain);

  // Peak detection
  applyStr("peak_detect", adv.m_libplaceboPeakDetect);
  applyStr("smoothing_period", adv.m_libplaceboSmoothingPeriod);
  applyStr("scene_threshold_low", adv.m_libplaceboSceneThresholdLow);
  applyStr("scene_threshold_high", adv.m_libplaceboSceneThresholdHigh);
  applyStr("peak_percentile", adv.m_libplaceboPeakPercentile);

  // Tone and gamut mapping
  applyStr("tone_mapping", adv.m_libplaceboToneMapping);
  applyStr("tone_mapping_param", adv.m_libplaceboToneMappingParam);
  applyStr("gamut_mapping", adv.m_libplaceboGamutMapping);
  applyStr("gamut_expansion", adv.m_libplaceboGamutExpansion);

  // Per-video tone mapping override (Kodi setting takes precedence over global)
  static constexpr const char* kToneMaps[] = {nullptr, "reinhard", "spline", "hable"};
  if (this->m_videoSettings.m_ToneMapMethod > 0 &&
      this->m_videoSettings.m_ToneMapMethod < VS_TONEMAPMETHOD_MAX)
    pl_options_set_str(m_plOpts, "tone_mapping", kToneMaps[this->m_videoSettings.m_ToneMapMethod]);

  // Dithering
  applyStr("dither", adv.m_libplaceboDither);
  applyStr("dither_method", adv.m_libplaceboDitherMethod);
  applyStr("dither_lut_size", adv.m_libplaceboDitherLutSize);

  // Deinterlacing
  applyStr("deinterlace", adv.m_libplaceboDeinterlace);
  applyStr("deinterlace_algo", adv.m_libplaceboDeinterlaceAlgo);

  // Misc
  applyStr("skip_anti_aliasing", adv.m_libplaceboSkipAntiAliasing);
  applyStr("disable_linear", adv.m_libplaceboDisableLinear);
  applyStr("disable_builtin_scalers", adv.m_libplaceboDisableBuiltinScalers);
  applyStr("force_dither", adv.m_libplaceboForceDither);
  applyStr("disable_fbos", adv.m_libplaceboDisableFbos);
}

template<typename TBase>
EShaderFormat CRendererPLBase<TBase>::GetShaderFormat()
{
  return SHADER_NONE;
}

template<typename TBase>
bool CRendererPLBase<TBase>::LoadShadersHook()
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
bool CRendererPLBase<TBase>::CreateTexture(int index)
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
void CRendererPLBase<TBase>::DeleteTexture(int index)
{
  ReleasePLBuffer(index);
  GLuint dummyTex = this->m_buffers[index].fields[0][0].id; // 0 == FIELD_FULL
  if (dummyTex > 0)
    glDeleteTextures(1, &dummyTex);
  this->m_buffers[index].fields[0][0].id = 0;
}

// ---------------------------------------------------------------------------
// UploadTexture — VAAPI / DRMPRIME / software paths
// ---------------------------------------------------------------------------

template<typename TBase>
bool CRendererPLBase<TBase>::UploadTexture(int index)
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
// ApplyHdrMetadata — shared HDR/DoVi metadata for all upload paths
// ---------------------------------------------------------------------------

template<typename TBase>
void CRendererPLBase<TBase>::ApplyHdrMetadata(PLBuffer& pb, const auto& b)
{
  if (b.m_srcColTransfer == AVCOL_TRC_SMPTEST2084 || b.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67)
  {
    pl_hdr_metadata& hdr = pb.colorSpace.hdr;
    hdr = {};
    if (b.hasDisplayMetadata)
    {
      const auto& m = b.displayMetadata;
      hdr.prim.red.x = av_q2d(m.display_primaries[0][0]);
      hdr.prim.red.y = av_q2d(m.display_primaries[0][1]);
      hdr.prim.green.x = av_q2d(m.display_primaries[1][0]);
      hdr.prim.green.y = av_q2d(m.display_primaries[1][1]);
      hdr.prim.blue.x = av_q2d(m.display_primaries[2][0]);
      hdr.prim.blue.y = av_q2d(m.display_primaries[2][1]);
      hdr.prim.white.x = av_q2d(m.white_point[0]);
      hdr.prim.white.y = av_q2d(m.white_point[1]);
      if (m.has_luminance)
      {
        hdr.max_luma = av_q2d(m.max_luminance);
        hdr.min_luma = av_q2d(m.min_luminance);
      }
    }
    if (b.hasLightMetadata)
    {
      hdr.max_cll = b.lightMetadata.MaxCLL;
      hdr.max_fall = b.lightMetadata.MaxFALL;
    }
  }
  if (!pl_hdr_metadata_equal(&b.plColorSpace.hdr, &pl_hdr_metadata_empty) ||
      b.plColorRepr.dovi != nullptr)
  {
    if (b.plColorRepr.dovi != nullptr)
    {
      pb.colorSpace = b.plColorSpace;
      pb.colorRepr = b.plColorRepr;
      pb.doviMetadata = b.plDoviMetadata;
      pb.colorRepr.dovi = &pb.doviMetadata;
    }
    else
    {
      pl_hdr_metadata_merge(&pb.colorSpace.hdr, &b.plColorSpace.hdr);
    }
  }
}

// ---------------------------------------------------------------------------
// UploadVAAPI — VAAPI: export surface as DRM PRIME 2 → EGLImage → GL tex → pl_opengl_wrap
// ---------------------------------------------------------------------------

#if defined(HAVE_LIBVA)
template<typename TBase>
bool CRendererPLBase<TBase>::UploadVAAPI(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];

  auto* vaaPic = dynamic_cast<VAAPI::CVaapiRenderPicture*>(buf.videoBuffer);
  if (!vaaPic)
    return false;

  if (!m_eglCreateImageKHR || !m_eglDestroyImageKHR || !m_glEGLImageTargetTexture2DOES)
  {
    CLog::Log(LOGERROR, "CRendererPLBase::UploadVAAPI - EGL interop not available");
    return false;
  }

  VASurfaceID surface = vaaPic->procPic.videoSurface;
  if (surface == VA_INVALID_ID && vaaPic->avFrame)
    surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(vaaPic->avFrame->data[3]));
  if (surface == VA_INVALID_ID)
  {
    CLog::Log(LOGERROR, "CRendererPLBase::UploadVAAPI - no valid VAAPI surface");
    return false;
  }

  VADisplay vadsp = vaaPic->vadsp;
  // Synchronise the VAAPI surface before we import its DMA-bufs.
  //
  // Priority order (best → worst):
  //  1. Implicit fencing (m_hasEGLModifiers): the kernel/EGL stack inserts a
  //     DMA-buf fence automatically when eglCreateImageKHR is called.  No
  //     explicit sync needed at all.
  //  2. GL_EXT_semaphore_fd (m_hasGLSemaphoreFd): export a sync-file fd from
  //     the DMA-buf via DMA_BUF_IOCTL_EXPORT_SYNC_FILE, then import it as a
  //     GL semaphore.  glWaitSemaphoreEXT (called in RenderHook) inserts the
  //     wait into the GPU command stream — the CPU thread returns immediately.
  //  3. vaSyncSurface(): CPU-blocking stall.  Used only when neither of the
  //     above is available.
  if (!m_hasEGLModifiers)
  {
    bool syncHandled = false;
#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
    if (m_hasGLSemaphoreFd)
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
    CLog::Log(LOGERROR, "CRendererPLBase::UploadVAAPI - vaExportSurfaceHandle failed: {}",
              vaErrorStr(vaStatus));
    return false;
  }

  plbuf.vaapiNumFds = static_cast<int>(
      std::min(desc.num_objects, static_cast<uint32_t>(std::size(plbuf.vaapiExportedFd))));
  for (int obj = 0; obj < plbuf.vaapiNumFds; ++obj)
    plbuf.vaapiExportedFd[obj] = desc.objects[obj].fd;

#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
  // GL_EXT_semaphore_fd path: export a read fence from the first DMA-buf
  // object and import it as a GL semaphore.  glWaitSemaphoreEXT (called in
  // RenderHook) submits the wait to the GPU command stream asynchronously —
  // the CPU is not stalled here.
  if (!m_hasEGLModifiers && m_hasGLSemaphoreFd && plbuf.vaapiNumFds > 0)
  {
    dma_buf_export_sync_file syncExport{};
    syncExport.flags = DMA_BUF_SYNC_READ;
    syncExport.fd = -1;
    if (ioctl(plbuf.vaapiExportedFd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &syncExport) == 0 &&
        syncExport.fd >= 0)
    {
      GLuint sem = 0;
      m_glGenSemaphoresEXT(1, &sem);
      // The fd is consumed (transferred to the GL driver) by the import call.
      m_glImportSemaphoreFdEXT(sem, GL_HANDLE_TYPE_SYNC_FD_EXT, syncExport.fd);
      plbuf.fenceSemaphore = sem;
      // Populate the texture barrier list: all VAAPI plane textures.
      plbuf.nFenceTextures = 0;
      for (int n = 0; n < static_cast<int>(std::size(plbuf.vaapiGLTex)); ++n)
      {
        if (plbuf.vaapiGLTex[n])
          plbuf.fenceTextures[plbuf.nFenceTextures++] = plbuf.vaapiGLTex[n];
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
                "CRendererPLBase::UploadVAAPI - eglCreateImageKHR failed for plane {} "
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
    m_glEGLImageTargetTexture2DOES(vaapiTexTarget, eglImage);
    // No glBindTexture(0) unbind needed: pl_opengl_wrap (called below) manages
    // the texture binding itself, and leaving a texture bound is harmless here.

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
      default:
        CLog::Log(LOGERROR,
                  "CRendererPLBase::UploadVAAPI - unsupported DRM fourcc 0x{:x} for plane {}",
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
      CLog::Log(LOGERROR, "CRendererPLBase::UploadVAAPI - pl_opengl_wrap failed for plane {}", i);
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

  ApplyHdrMetadata(plbuf, buf);

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
bool CRendererPLBase<TBase>::UploadDRMPRIME(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];

  auto* drmBuf = dynamic_cast<CVideoBufferDRMPRIME*>(buf.videoBuffer);
  if (!drmBuf)
    return false;

  m_drmTextures[index].Unmap(); // defensive — no-op if not mapped
  if (!m_drmTextures[index].Map(drmBuf))
  {
    CLog::Log(LOGERROR, "CRendererPLBase::UploadDRMPRIME - CDRMPRIMETexture::Map failed");
    return false;
  }

  pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  GLuint glTex = m_drmTextures[index].GetTexture();
  CSizeInt sz = m_drmTextures[index].GetTextureSize();

  pl_opengl_wrap_params wp{};
  wp.texture = glTex;
  wp.target = GL_TEXTURE_EXTERNAL_OES;
  wp.iformat = GL_RGBA8;
  wp.width = sz.Width();
  wp.height = sz.Height();

  plbuf.tex[0] = pl_opengl_wrap(gpu, &wp);
  if (!plbuf.tex[0])
  {
    CLog::Log(LOGERROR, "CRendererPLBase::UploadDRMPRIME - pl_opengl_wrap failed");
    m_drmTextures[index].Unmap();
    return false;
  }

  plbuf.planes[0] = {};
  plbuf.planes[0].texture = plbuf.tex[0];
  plbuf.planes[0].components = 3;
  plbuf.planes[0].component_mapping[0] = PL_CHANNEL_R;
  plbuf.planes[0].component_mapping[1] = PL_CHANNEL_G;
  plbuf.planes[0].component_mapping[2] = PL_CHANNEL_B;
  plbuf.planes[0].component_mapping[3] = PL_CHANNEL_NONE;
  plbuf.num_planes = 1;

  // The GPU driver applies the YCbCr→RGB matrix when the OES texture is sampled,
  // yielding RGB in the source primaries/transfer. Tell libplacebo this is RGB.
  plbuf.colorRepr.sys = PL_COLOR_SYSTEM_RGB;
  plbuf.colorRepr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  plbuf.colorSpace.primaries = pl_primaries_from_av(buf.m_srcPrimaries);
  plbuf.colorSpace.transfer = pl_transfer_from_av(buf.m_srcColTransfer);

  ApplyHdrMetadata(plbuf, buf);

  plbuf.iFlags = buf.iFlags;

#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
  // GPU-side fence for the V4L2 / DRMPRIME decode path (e.g. bcm2835-codec on
  // RPi5).  The VC4/V3D pipeline sets a DMA-buf read fence on the output
  // buffer when decode completes; we export it as a sync-file and import it
  // as a GL semaphore.  glWaitSemaphoreEXT (called in RenderHook) inserts the
  // wait into the GPU command stream without stalling the CPU.
  //
  // Skipped when m_hasEGLModifiers is true: in that case eglCreateImageKHR
  // (called inside CDRMPRIMETexture::Map above) already attached the DMA-buf
  // reservation fence implicitly — adding an explicit semaphore is redundant.
  if (m_hasGLSemaphoreFd && !m_hasEGLModifiers)
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
        GLuint sem = 0;
        m_glGenSemaphoresEXT(1, &sem);
        m_glImportSemaphoreFdEXT(sem, GL_HANDLE_TYPE_SYNC_FD_EXT, syncExport.fd);
        plbuf.fenceSemaphore = sem;
        plbuf.fenceTextures[0] = glTex;
        plbuf.nFenceTextures = 1;
      }
    }
  }
#endif

  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}

// ---------------------------------------------------------------------------
// UploadSoftware — CPU→GPU upload via pl_upload_plane
// ---------------------------------------------------------------------------

template<typename TBase>
bool CRendererPLBase<TBase>::UploadSoftware(int index, PLBuffer& plbuf)
{
  auto& buf = this->m_buffers[index];

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
              "CRendererPLBase::UploadSoftware - unsupported pixel format {} (buf reports {})",
              static_cast<int>(fmt), static_cast<int>(buf.videoBuffer->GetFormat()));
    return false;
  }

  pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;

  // Use the AVPixFmtDescriptor (m_format is AVPixelFormat, set from videoBuffer->GetFormat())
  // to derive the correct per-plane chroma shifts.  This handles 4:2:0, 4:2:2, and 4:4:4
  // without hardcoding.  Fall back to shift=1 (4:2:0) if the descriptor is unavailable.
  //
  // NOTE: Do NOT use AV_CEIL_RSHIFT(a, b) with a runtime b.  When b is not a compile-time
  // constant, the macro takes the path -(-(a) >> b) which is implementation-defined for
  // signed integers and can produce wrong results.  Use the explicit ceiling formula instead.
  const AVPixFmtDescriptor* fmtDesc = av_pix_fmt_desc_get(fmt);
  const int chromaShiftW = fmtDesc ? static_cast<int>(fmtDesc->log2_chroma_w) : 1;
  const int chromaShiftH = fmtDesc ? static_cast<int>(fmtDesc->log2_chroma_h) : 1;

  for (int n = 0; n < plbuf.num_planes; ++n)
  {
    pdata[n].pixels = src[n];
    pdata[n].row_stride = srcStrides[n];
    pdata[n].width = (n > 0) ? (this->m_sourceWidth + (1 << chromaShiftW) - 1) >> chromaShiftW
                             : this->m_sourceWidth;
    pdata[n].height = (n > 0) ? (this->m_sourceHeight + (1 << chromaShiftH) - 1) >> chromaShiftH
                              : this->m_sourceHeight;

    if (!pl_upload_plane(gpu, &plbuf.planes[n], &plbuf.tex[n], &pdata[n]))
    {
      CLog::Log(LOGERROR, "CRendererPLBase::UploadSoftware - pl_upload_plane failed for plane {}",
                n);
      return false;
    }

    // OpenGL's coordinate system is bottom-up; top-down memory uploads are inverted.
    plbuf.planes[n].flipped = true;
  }

  plbuf.colorSpace.primaries = pl_primaries_from_av(buf.m_srcPrimaries);
  plbuf.colorSpace.transfer = pl_transfer_from_av(buf.m_srcColTransfer);
  plbuf.colorRepr.sys = pl_system_from_av(buf.m_srcColSpace);
  if (plbuf.colorRepr.sys == PL_COLOR_SYSTEM_UNKNOWN)
    plbuf.colorRepr.sys = pl_color_system_guess_ycbcr(this->m_sourceWidth, this->m_sourceHeight);
  plbuf.colorRepr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  plbuf.colorRepr.bits = bits;

  ApplyHdrMetadata(plbuf, buf);

  plbuf.iFlags = buf.iFlags;
  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}

// ---------------------------------------------------------------------------
// Main render hook
// ---------------------------------------------------------------------------

template<typename TBase>
bool CRendererPLBase<TBase>::RenderHook(int idx)
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

  frameIn.rotation = RotationFromOrientation(this->m_renderOrientation);
  frameIn.field = PL_FIELD_NONE;

  CRect src, dst, view;
  this->GetVideoRect(src, dst, view);
  int viewW = static_cast<int>(view.Width());
  int viewH = static_cast<int>(view.Height());
  if (viewW <= 0 || viewH <= 0)
    return false;

  GLint currentFbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &currentFbo);
  const auto fboId = static_cast<unsigned int>(currentFbo);

  // Re-wrap the framebuffer only when it changes.  pl_opengl_wrap/pl_tex_destroy
  // on every frame is expensive: some drivers flush pending GPU work at this point
  // and libplacebo discards cached per-target state, forcing a full re-initialisation
  // on the next pl_render_image[_mix] call.
  if (!m_cachedFboTex || fboId != m_cachedFboId || viewW != m_cachedFboW || viewH != m_cachedFboH)
  {
    if (m_cachedFboTex)
      pl_tex_destroy(gpu, &m_cachedFboTex);

    pl_opengl_wrap_params wrapParams{};
    wrapParams.framebuffer = fboId;
    wrapParams.width = viewW;
    wrapParams.height = viewH;
    wrapParams.iformat = GL_RGBA8;

    m_cachedFboTex = pl_opengl_wrap(gpu, &wrapParams);
    if (!m_cachedFboTex)
    {
      CLog::Log(LOGERROR, "CRendererPLBase::RenderHook - failed to wrap GL framebuffer");
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
  }
  else
  {
    frameOut.color.primaries = PL_COLOR_PRIM_BT_709;
    frameOut.color.transfer = PL_COLOR_TRC_BT_1886;
  }
  frameOut.repr.sys = PL_COLOR_SYSTEM_RGB;
  frameOut.repr.levels = CServiceBroker::GetWinSystem()->UseLimitedColor() ? PL_COLOR_LEVELS_LIMITED
                                                                           : PL_COLOR_LEVELS_FULL;

  pl_render_params params = m_plOpts->params;
  params.border = PL_CLEAR_SKIP;

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
  // Cache the result: glGetIntegerv(GL_VERTEX_ARRAY_BINDING) serialises the GPU
  // command stream on tile-based GPUs (Mali/Adreno/PowerVR) because the driver
  // must flush in-flight work before reading the register.  Kodi's VAO is created
  // once at startup and never changes, so we query it only on the first render.
  if (!m_kodiVaoCached)
  {
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_kodiVAO);
    m_kodiVaoCached = true;
  }
  const GLint savedVAO = m_kodiVAO;

  // GPU-side DMA-buf fence wait (GL_EXT_semaphore_fd).
  // UploadTexture() may have imported a DMA-buf sync-file fence for VAAPI or
  // DRMPRIME frames.  Drain all pending semaphores here — after pl_queue_update
  // has completed its MapCallback chain, before any pl_render_image[_mix]
  // command touches the textures.  glWaitSemaphoreEXT inserts the wait into the
  // GPU command stream; the CPU thread returns immediately.
  if (m_hasGLSemaphoreFd)
  {
    for (auto& fbuf : m_plBuffers)
    {
      if (!fbuf.fenceSemaphore)
        continue;

      GLenum layouts[3] = {GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT};
      m_glWaitSemaphoreEXT(fbuf.fenceSemaphore, 0, nullptr,
                           static_cast<GLuint>(fbuf.nFenceTextures), fbuf.fenceTextures, layouts);
      m_glDeleteSemaphoresEXT(1, &fbuf.fenceSemaphore);
      fbuf.fenceSemaphore = 0;
      fbuf.nFenceTextures = 0;
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
    qparams.timeout = 0;

    pl_frame_mix mix{};
    const auto status = pl_queue_update(m_plQueue, &mix, &qparams);
    if (status == PL_QUEUE_OK || status == PL_QUEUE_MORE) [[likely]]
    {
      if (!pl_render_image_mix(renderer, &mix, &frameOut, &params))
        CLog::Log(LOGWARNING, "CRendererPLBase::RenderHook - pl_render_image_mix failed");
      rendered = true;
    }
    else if (status == PL_QUEUE_ERR) [[unlikely]]
    {
      CLog::Log(LOGWARNING, "CRendererPLBase::RenderHook - pl_queue_update error");
    }
  }
  if (!rendered)
  {
    if (!pl_render_image(renderer, &frameIn, &frameOut, &params))
      CLog::Log(LOGWARNING, "CRendererPLBase::RenderHook - pl_render_image failed");
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
void CRendererPLBase<TBase>::ReleasePLBuffer(int index)
{
  PLBuffer& plbuf = m_plBuffers[index];

#if defined(HAVE_LIBVA)
  if (m_isVAAPI)
  {
    // Drop any pending fence semaphore.  If the wait hasn't fired yet the
    // semaphore object is simply deleted; the associated sync object is
    // released by the GL driver without blocking the CPU.
    if (plbuf.fenceSemaphore && m_glDeleteSemaphoresEXT)
    {
      m_glDeleteSemaphoresEXT(1, &plbuf.fenceSemaphore);
      plbuf.fenceSemaphore = 0;
      plbuf.nFenceTextures = 0;
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

  if (!plbuf.loaded)
    return;

  // Drop any pending fence semaphore (DRMPRIME path).
  if (plbuf.fenceSemaphore && m_glDeleteSemaphoresEXT)
  {
    m_glDeleteSemaphoresEXT(1, &plbuf.fenceSemaphore);
    plbuf.fenceSemaphore = 0;
    plbuf.nFenceTextures = 0;
  }

  m_drmTextures[index].Unmap();

  pl_gpu gpu = PL::PLInstance::Get()->m_plGpu;
  for (int n = 0; n < plbuf.num_planes; ++n)
  {
    if (plbuf.tex[n])
    {
      pl_tex_destroy(gpu, &plbuf.tex[n]);
      plbuf.tex[n] = nullptr;
    }
    plbuf.planes[n] = {};
  }
  plbuf.num_planes = 0;
  plbuf.loaded = false;
}
