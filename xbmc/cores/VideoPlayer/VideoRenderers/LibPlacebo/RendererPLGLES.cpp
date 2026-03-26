/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererPLGLES.h"

#include "PlHelper.h"
#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#include "windowing/linux/WinSystemEGL.h"

#if defined(HAVE_LIBVA)
#include "cores/VideoPlayer/DVDCodecs/Video/VAAPI.h"
#include <drm_fourcc.h>
#include <va/va_drmcommon.h>
#endif

#include "system_egl.h"
#include "system_gl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <libplacebo/utils/libav.h>
#include <unistd.h>


// ---------------------------------------------------------------------------
// Static factory
// ---------------------------------------------------------------------------

CBaseRenderer* CRendererPLGLES::Create(CVideoBuffer* buffer)
{
  if (!buffer)
    return nullptr;

  // Accept DRMPRIME DMA-buf buffers, VAAPI surfaces, and any software-decoded
  // buffer whose pixel format libplacebo can handle via pl_upload_plane().
  // Other hardware-decoded buffers report AV_PIX_FMT_NONE and fall through to
  // the platform-native renderer.
  bool isDRMPRIME = (dynamic_cast<CVideoBufferDRMPRIME*>(buffer) != nullptr);
#if defined(HAVE_LIBVA)
  bool isVAAPI = (dynamic_cast<VAAPI::CVaapiRenderPicture*>(buffer) != nullptr);
#else
  constexpr bool isVAAPI = false;
#endif
  pl_bit_encoding bits{};
  pl_plane_data pdata[4]{};
  bool isSWFormat = (pl_plane_data_from_pixfmt(pdata, &bits, buffer->GetFormat()) > 0);
  if (!isDRMPRIME && !isVAAPI && !isSWFormat)
  {
    CLog::Log(LOGDEBUG,
              "CRendererPLGLES::Create - buffer format {} not supported by libplacebo upload path, "
              "skipping",
              static_cast<int>(buffer->GetFormat()));
    return nullptr;
  }

  auto* inst = PL::PLInstance::Get().get();
  if (!inst->Init())
  {
    CLog::Log(LOGERROR, "CRendererPLGLES::Create - PLInstance::Init() failed");
    return nullptr;
  }

  return new CRendererPLGLES();
}

bool CRendererPLGLES::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("libplacebo", CRendererPLGLES::Create);
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

CRendererPLGLES::CRendererPLGLES()
{
  m_plOpts = pl_options_alloc(PL::PLInstance::Get()->m_plLog);
}

CRendererPLGLES::~CRendererPLGLES()
{
  for (int i = 0; i < NUM_BUFFERS; ++i)
    ReleasePLBuffer(i);
  pl_options_free(&m_plOpts);
  PL::PLInstance::Get()->Reset();
}

// ---------------------------------------------------------------------------
// Configure
// ---------------------------------------------------------------------------

bool CRendererPLGLES::Configure(const VideoPicture& picture,
                                float fps,
                                unsigned int orientation)
{
  if (!CLinuxRendererGLES::Configure(picture, fps, orientation))
    return false;

  m_format = picture.videoBuffer->GetFormat();
  m_isDRMPRIME = (dynamic_cast<CVideoBufferDRMPRIME*>(picture.videoBuffer) != nullptr);

  if (m_isDRMPRIME)
  {
    auto* winEGL =
        dynamic_cast<KODI::WINDOWING::LINUX::CWinSystemEGL*>(CServiceBroker::GetWinSystem());
    EGLDisplay eglDisplay = winEGL ? winEGL->GetEGLDisplay() : EGL_NO_DISPLAY;
    for (int i = 0; i < NUM_BUFFERS; ++i)
      m_drmTextures[i].Init(eglDisplay);
  }

#if defined(HAVE_LIBVA)
  m_isVAAPI = (dynamic_cast<VAAPI::CVaapiRenderPicture*>(picture.videoBuffer) != nullptr);
  if (m_isVAAPI)
  {
    m_eglDisplay = eglGetCurrentDisplay();
    m_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    m_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    m_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    m_hasEGLModifiers = (eglGetProcAddress("eglQueryDmaBufModifiersEXT") != nullptr);
  }
#endif

  m_colorSpace = pl_color_space{};
  m_colorSpace.primaries = pl_primaries_from_av(picture.color_primaries);
  m_colorSpace.transfer = pl_transfer_from_av(picture.color_transfer);

  m_chromaLocation = pl_chroma_from_av(picture.chroma_position);

  return true;
}

bool CRendererPLGLES::ConfigChanged(const VideoPicture& picture)
{
  if (picture.videoBuffer->GetFormat() != m_format)
    return true;
  return false;
}

// ---------------------------------------------------------------------------
// Feature / scaling support
// ---------------------------------------------------------------------------

bool CRendererPLGLES::Supports(ERENDERFEATURE feature) const
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

bool CRendererPLGLES::Supports(ESCALINGMETHOD method) const
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
// UpdateVideoFilter — called when video settings change
// ---------------------------------------------------------------------------

void CRendererPLGLES::UpdateVideoFilter()
{
  CLinuxRendererGLES::UpdateVideoFilter();

  // Reset to libplacebo's own defaults first
  pl_options_reset(m_plOpts, nullptr);

  // Apply global libplacebo settings from advancedsettings.xml.
  // Empty strings are not passed; non-empty values override libplacebo's defaults.
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

  // Per-video tone mapping override (existing Kodi setting takes precedence)
  static const char* const kToneMaps[] = {nullptr, "reinhard", "spline", "hable"};
  if (m_videoSettings.m_ToneMapMethod > 0 &&
      m_videoSettings.m_ToneMapMethod < VS_TONEMAPMETHOD_MAX)
    pl_options_set_str(m_plOpts, "tone_mapping", kToneMaps[m_videoSettings.m_ToneMapMethod]);

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

EShaderFormat CRendererPLGLES::GetShaderFormat()
{
  return SHADER_NONE;
}

// ---------------------------------------------------------------------------
// Shader hook — tell LinuxRendererGLES we handle rendering ourselves
// ---------------------------------------------------------------------------

bool CRendererPLGLES::LoadShadersHook()
{
  m_renderMethod = RENDER_GLSL;
  return true;
}

// ---------------------------------------------------------------------------
// Texture lifecycle
// ---------------------------------------------------------------------------

bool CRendererPLGLES::CreateTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  YuvImage& im = buf.image;
  memset(&im, 0, sizeof(im));
  im.height = m_sourceHeight;
  im.width = m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;
  buf.fields[FIELD_FULL][0].id = 1;
  return true;
}

void CRendererPLGLES::DeleteTexture(int index)
{
  ReleasePLBuffer(index);
  m_buffers[index].fields[FIELD_FULL][0].id = 0;
}

bool CRendererPLGLES::UploadTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  if (!buf.videoBuffer)
    return false;

  PLBuffer& plbuf = m_plBuffers[index];
  ReleasePLBuffer(index);

#if defined(HAVE_LIBVA)
  // --- VAAPI path: export surface as DRM PRIME 2 → EGLImage → GL_TEXTURE_2D → pl_opengl_wrap ---
  if (m_isVAAPI)
  {
    auto* vaaPic = dynamic_cast<VAAPI::CVaapiRenderPicture*>(buf.videoBuffer);
    if (!vaaPic)
      return false;

    if (!m_eglCreateImageKHR || !m_eglDestroyImageKHR || !m_glEGLImageTargetTexture2DOES)
    {
      CLog::Log(LOGERROR,
                "CRendererPLGLES::UploadTexture - EGL interop functions not available for VAAPI");
      return false;
    }

    // procPic path is primary; avFrame path provides the surface via data[3]
    VASurfaceID surface = vaaPic->procPic.videoSurface;
    if (surface == VA_INVALID_ID && vaaPic->avFrame)
      surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(vaaPic->avFrame->data[3]));
    if (surface == VA_INVALID_ID)
    {
      CLog::Log(LOGERROR, "CRendererPLGLES::UploadTexture - no valid VAAPI surface");
      return false;
    }

    VADisplay vadsp = vaaPic->vadsp;
    vaSyncSurface(vadsp, surface);

    VADRMPRIMESurfaceDescriptor desc{};
    VAStatus vaStatus = vaExportSurfaceHandle(vadsp, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &desc);
    if (vaStatus != VA_STATUS_SUCCESS)
    {
      CLog::Log(LOGERROR, "CRendererPLGLES::UploadTexture - vaExportSurfaceHandle failed: {}",
                vaErrorStr(vaStatus));
      return false;
    }

    // Take ownership of the exported fds; EGL internally dups them in eglCreateImageKHR
    plbuf.vaapiNumFds = static_cast<int>(std::min(desc.num_objects,
        static_cast<uint32_t>(std::size(plbuf.vaapiExportedFd))));
    for (int obj = 0; obj < plbuf.vaapiNumFds; ++obj)
      plbuf.vaapiExportedFd[obj] = desc.objects[obj].fd;

    pl_gpu gpu = PL::PLInstance::Get()->GetGpu();
    bool success = true;
    const uint32_t numLayers = std::min(desc.num_layers, 3u);

    for (uint32_t i = 0; i < numLayers && success; ++i)
    {
      const auto& layer = desc.layers[i];
      const auto& object = desc.objects[layer.object_index[0]];

      // Derive per-plane dimensions: layer 0 = full frame, layer 1 = 4:2:0 chroma
      const EGLint planeW =
          (i == 0) ? static_cast<EGLint>(desc.width) : (static_cast<EGLint>(desc.width) + 1) / 2;
      const EGLint planeH =
          (i == 0) ? static_cast<EGLint>(desc.height) : (static_cast<EGLint>(desc.height) + 1) / 2;

      // Build EGL attribute list (max 6 static + 2 modifier = 8 pairs + EGL_NONE)
      EGLint attribs[17];
      EGLint* a = attribs;
      *a++ = EGL_LINUX_DRM_FOURCC_EXT;      *a++ = static_cast<EGLint>(layer.drm_format);
      *a++ = EGL_WIDTH;                      *a++ = planeW;
      *a++ = EGL_HEIGHT;                     *a++ = planeH;
      *a++ = EGL_DMA_BUF_PLANE0_FD_EXT;     *a++ = object.fd;
      *a++ = EGL_DMA_BUF_PLANE0_OFFSET_EXT; *a++ = static_cast<EGLint>(layer.offset[0]);
      *a++ = EGL_DMA_BUF_PLANE0_PITCH_EXT;  *a++ = static_cast<EGLint>(layer.pitch[0]);
      if (m_hasEGLModifiers && object.drm_format_modifier != DRM_FORMAT_MOD_INVALID)
      {
        *a++ = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        *a++ = static_cast<EGLint>(object.drm_format_modifier & 0xFFFFFFFFu);
        *a++ = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        *a++ = static_cast<EGLint>(object.drm_format_modifier >> 32);
      }
      *a++ = EGL_NONE;

      EGLImageKHR eglImage = m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT,
          EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
      if (!eglImage)
      {
        CLog::Log(LOGERROR,
                  "CRendererPLGLES::UploadTexture - eglCreateImageKHR failed for VAAPI plane {} "
                  "(EGL error 0x{:x})", i, static_cast<unsigned>(eglGetError()));
        success = false;
        break;
      }
      plbuf.vaapiEGLImage[i] = eglImage;

      // Bind EGLImage to GL_TEXTURE_2D via GL_OES_EGL_image.
      // On GLES, GL_TEXTURE_EXTERNAL_OES is also an option, but GL_TEXTURE_2D works
      // and keeps the import path symmetric with the desktop GL renderer.
      glGenTextures(1, &plbuf.vaapiGLTex[i]);
      glBindTexture(GL_TEXTURE_2D, plbuf.vaapiGLTex[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, eglImage);
      glBindTexture(GL_TEXTURE_2D, 0);

      // Map DRM fourcc → GL internal format for pl_opengl_wrap.
      // pl_assert(params->iformat) requires iformat != 0 when texture != 0.
      GLenum glIformat = 0;
      switch (layer.drm_format)
      {
        case DRM_FORMAT_R8:     glIformat = GL_R8;   break;
        case DRM_FORMAT_GR88:   glIformat = GL_RG8;  break;
        case DRM_FORMAT_R16:    glIformat = GL_R16;  break;
        case DRM_FORMAT_GR1616: glIformat = GL_RG16; break;
        default:
          CLog::Log(LOGERROR,
                    "CRendererPLGLES::UploadTexture - unsupported DRM fourcc 0x{:x} for VAAPI "
                    "plane {}", layer.drm_format, i);
          success = false;
          break;
      }
      if (!success)
        break;

      pl_opengl_wrap_params wp{};
      wp.texture = plbuf.vaapiGLTex[i];
      wp.target  = GL_TEXTURE_EXTERNAL_OES;
      wp.iformat = static_cast<int>(glIformat);
      wp.width   = planeW;
      wp.height  = planeH;

      plbuf.tex[i] = pl_opengl_wrap(gpu, &wp);
      if (!plbuf.tex[i])
      {
        CLog::Log(LOGERROR,
                  "CRendererPLGLES::UploadTexture - pl_opengl_wrap failed for VAAPI plane {}", i);
        success = false;
      }
    }

    if (!success)
    {
      ReleasePLBuffer(index);
      return false;
    }

    // Layer 0 = Y (luma), layer 1 = UV (chroma, interleaved Cb+Cr — NV12/P010/P016)
    plbuf.num_planes = static_cast<int>(numLayers);

    plbuf.planes[0] = {};
    plbuf.planes[0].texture = plbuf.tex[0];
    plbuf.planes[0].components = 1;
    plbuf.planes[0].component_mapping[0] = PL_CHANNEL_Y;
    plbuf.planes[0].component_mapping[1] = PL_CHANNEL_NONE;
    plbuf.planes[0].component_mapping[2] = PL_CHANNEL_NONE;
    plbuf.planes[0].component_mapping[3] = PL_CHANNEL_NONE;
    // EGLImage DMA-buf: row 0 in memory → GL texcoord t=0 (bottom in GL convention)
    // but row 0 is the top of the video → texture appears flipped; correct with flipped=true.
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

    // libplacebo performs YCbCr→RGB with the correct BT.601/709/2020 matrix
    plbuf.colorRepr.sys = pl_system_from_av(buf.m_srcColSpace);
    // PL_COLOR_SYSTEM_UNKNOWN causes libplacebo to skip the YCbCr→RGB matrix
    // (renderer.c detect_plane_type falls through to PLANE_RGB), producing
    // wildly wrong colours (e.g. blue → green). Streams without VUI colour-
    // matrix signalling report AVCOL_SPC_UNSPECIFIED which maps to UNKNOWN.
    if (plbuf.colorRepr.sys == PL_COLOR_SYSTEM_UNKNOWN)
      plbuf.colorRepr.sys = pl_color_system_guess_ycbcr(m_sourceWidth, m_sourceHeight);
    plbuf.colorRepr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
    plbuf.colorSpace.primaries = pl_primaries_from_av(buf.m_srcPrimaries);
    plbuf.colorSpace.transfer = pl_transfer_from_av(buf.m_srcColTransfer);

    // Set bit encoding so libplacebo correctly normalises sub-10/16-bit values.
    // Intel VAAPI P010 exports DRM_FORMAT_R16/GR1616 with 10 significant bits
    // left-shifted to bits [15:6] of each 16-bit LE word (bit_shift=6).
    // P016 uses the full 16-bit range. NV12 uses the default (8-bit, zeros).
    switch (desc.fourcc)
    {
      case VA_FOURCC_P010: plbuf.colorRepr.bits = {16, 10, 6}; break;
      case VA_FOURCC_P016: plbuf.colorRepr.bits = {16, 16, 0}; break;
      default: break;
    }

    if (buf.m_srcColTransfer == AVCOL_TRC_SMPTEST2084 ||
        buf.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67)
    {
      pl_hdr_metadata& hdr = plbuf.colorSpace.hdr;
      hdr = {};
      if (buf.hasDisplayMetadata)
      {
        const AVMasteringDisplayMetadata& m = buf.displayMetadata;
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
      if (buf.hasLightMetadata)
      {
        hdr.max_cll = buf.lightMetadata.MaxCLL;
        hdr.max_fall = buf.lightMetadata.MaxFALL;
      }
    }

    if (buf.plColorSpace.hdr.max_luma > 0.0f || buf.plColorRepr.dovi != nullptr)
    {
      plbuf.colorSpace = buf.plColorSpace;
      plbuf.colorRepr = buf.plColorRepr;
      plbuf.doviMetadata = buf.plDoviMetadata;
      plbuf.colorRepr.dovi = &plbuf.doviMetadata;
    }

    plbuf.iFlags = buf.iFlags;
    plbuf.loaded = true;
    buf.loaded = true;
    return true;
  }
#endif

  // --- Hardware path: DRMPRIME DMA-buf → EGLImage → OES texture → pl_tex ---
  auto* drmBuf = dynamic_cast<CVideoBufferDRMPRIME*>(buf.videoBuffer);
  if (drmBuf)
  {
    m_drmTextures[index].Unmap();
    if (!m_drmTextures[index].Map(drmBuf))
    {
      CLog::Log(LOGERROR, "CRendererPLGLES::UploadTexture - CDRMPRIMETexture::Map failed");
      return false;
    }

    pl_gpu gpu = PL::PLInstance::Get()->GetGpu();
    GLuint glTex = m_drmTextures[index].GetTexture();
    CSizeInt sz = m_drmTextures[index].GetTextureSize();

    pl_opengl_wrap_params wp{};
    wp.texture = glTex;
    wp.target = GL_TEXTURE_EXTERNAL_OES;
    // pl_assert(params->iformat) fires when texture != 0 and iformat == 0.
    // Use GL_RGBA8 for format metadata (component count/type for shader gen).
    // The actual sampling precision is determined by the EGL driver; libplacebo
    // uses iformat only to determine the GLSL sampler prefix here.
    wp.iformat = GL_RGBA8;
    wp.width = sz.Width();
    wp.height = sz.Height();

    plbuf.tex[0] = pl_opengl_wrap(gpu, &wp);
    if (!plbuf.tex[0])
    {
      CLog::Log(LOGERROR,
                "CRendererPLGLES::UploadTexture - pl_opengl_wrap failed for DRMPRIME texture");
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

    // Mesa V3D applies the BT.601/709/2020 YCbCr→RGB matrix when the OES
    // texture is sampled. The result is RGB values still encoded in the
    // source primaries/transfer (BT.1886/PQ/HLG), so libplacebo can
    // perform correct HDR tone-mapping.
    plbuf.colorRepr.sys = PL_COLOR_SYSTEM_RGB;
    plbuf.colorRepr.levels =
        buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
    plbuf.colorSpace.primaries = pl_primaries_from_av(buf.m_srcPrimaries);
    plbuf.colorSpace.transfer = pl_transfer_from_av(buf.m_srcColTransfer);

    // HDR static metadata
    if (buf.m_srcColTransfer == AVCOL_TRC_SMPTEST2084 ||
        buf.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67)
    {
      pl_hdr_metadata& hdr = plbuf.colorSpace.hdr;
      hdr = {};
      if (buf.hasDisplayMetadata)
      {
        const AVMasteringDisplayMetadata& m = buf.displayMetadata;
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
      if (buf.hasLightMetadata)
      {
        hdr.max_cll = buf.lightMetadata.MaxCLL;
        hdr.max_fall = buf.lightMetadata.MaxFALL;
      }
    }

    // Per-frame DOVI color metadata takes priority over static fields
    if (buf.plColorSpace.hdr.max_luma > 0.0f || buf.plColorRepr.dovi != nullptr)
    {
      plbuf.colorSpace = buf.plColorSpace;
      plbuf.colorRepr = buf.plColorRepr;
      plbuf.doviMetadata = buf.plDoviMetadata;
      plbuf.colorRepr.dovi = &plbuf.doviMetadata;
    }

    plbuf.iFlags = buf.iFlags;
    plbuf.loaded = true;
    buf.loaded = true;
    return true;
  }

  // --- Software path: CPU→GPU upload via pl_upload_plane ---
  uint8_t* src[3]{};
  int srcStrides[3]{};
  buf.videoBuffer->GetPlanes(src);
  buf.videoBuffer->GetStrides(srcStrides);

  pl_bit_encoding bits{};
  pl_plane_data pdata[4]{};
  // Use the format cached during Configure() — calling GetFormat() directly on the
  // buffer can return AV_PIX_FMT_NONE for certain codec/pool buffer types.
  const AVPixelFormat fmt = m_format;
  plbuf.num_planes = pl_plane_data_from_pixfmt(pdata, &bits, fmt);
  if (plbuf.num_planes <= 0)
  {
    CLog::Log(LOGERROR, "CRendererPLGLES::UploadTexture - unsupported pixel format {} (buf reports {})",
              (int)fmt, (int)buf.videoBuffer->GetFormat());
    return false;
  }

  pl_gpu gpu = PL::PLInstance::Get()->GetGpu();

  for (int n = 0; n < plbuf.num_planes; ++n)
  {
    pdata[n].pixels = src[n];
    pdata[n].row_stride = srcStrides[n];
    pdata[n].width = (n > 0) ? m_sourceWidth >> 1 : m_sourceWidth;
    pdata[n].height = (n > 0) ? m_sourceHeight >> 1 : m_sourceHeight;

    if (!pl_upload_plane(gpu, &plbuf.planes[n], &plbuf.tex[n], &pdata[n]))
    {
      CLog::Log(LOGERROR, "CRendererPLGLES::UploadTexture - pl_upload_plane failed for plane {}",
                n);
      return false;
    }
  }

  plbuf.colorSpace.primaries = pl_primaries_from_av(buf.m_srcPrimaries);
  plbuf.colorSpace.transfer = pl_transfer_from_av(buf.m_srcColTransfer);
  plbuf.colorRepr.sys = pl_system_from_av(buf.m_srcColSpace);
  if (plbuf.colorRepr.sys == PL_COLOR_SYSTEM_UNKNOWN)
    plbuf.colorRepr.sys = pl_color_system_guess_ycbcr(m_sourceWidth, m_sourceHeight);
  plbuf.colorRepr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  plbuf.colorRepr.bits = bits;

  if (buf.m_srcColTransfer == AVCOL_TRC_SMPTEST2084 ||
      buf.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67)
  {
    pl_hdr_metadata& hdr = plbuf.colorSpace.hdr;
    hdr = {};
    if (buf.hasDisplayMetadata)
    {
      const AVMasteringDisplayMetadata& m = buf.displayMetadata;
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
    if (buf.hasLightMetadata)
    {
      hdr.max_cll = buf.lightMetadata.MaxCLL;
      hdr.max_fall = buf.lightMetadata.MaxFALL;
    }
  }

  // Per-frame DOVI color metadata takes priority over static fields
  if (buf.plColorSpace.hdr.max_luma > 0.0f || buf.plColorRepr.dovi != nullptr)
  {
    plbuf.colorSpace = buf.plColorSpace;
    plbuf.colorRepr = buf.plColorRepr;
    plbuf.doviMetadata = buf.plDoviMetadata;
    plbuf.colorRepr.dovi = &plbuf.doviMetadata;
  }

  plbuf.iFlags = buf.iFlags;
  plbuf.loaded = true;
  buf.loaded = true;
  return true;
}

// ---------------------------------------------------------------------------
// Main render hook
// ---------------------------------------------------------------------------

bool CRendererPLGLES::RenderHook(int idx)
{
  PLBuffer& plbuf = m_plBuffers[idx];
  if (!plbuf.loaded)
    return false;

  pl_gpu gpu = PL::PLInstance::Get()->GetGpu();
  pl_renderer renderer = PL::PLInstance::Get()->GetRenderer();

  // --- Build input frame ---
  pl_frame frameIn{};
  frameIn.num_planes = plbuf.num_planes;
  for (int n = 0; n < plbuf.num_planes; ++n)
    frameIn.planes[n] = plbuf.planes[n];
  frameIn.color = plbuf.colorSpace;
  frameIn.repr = plbuf.colorRepr;
  pl_frame_set_chroma_location(&frameIn, m_chromaLocation);

  switch (m_renderOrientation)
  {
    case 90:
      frameIn.rotation = PL_ROTATION_270;
      break;
    case 180:
      frameIn.rotation = PL_ROTATION_180;
      break;
    case 270:
      frameIn.rotation = PL_ROTATION_90;
      break;
    default:
      frameIn.rotation = PL_ROTATION_0;
      break;
  }

  // --- Interlace field tagging ---
  if (plbuf.iFlags & DVP_FLAG_INTERLACED)
  {
    bool topFirst = (plbuf.iFlags & DVP_FLAG_TOP_FIELD_FIRST) != 0;
    frameIn.field = topFirst ? PL_FIELD_TOP : PL_FIELD_BOTTOM;
    frameIn.first_field = topFirst ? PL_FIELD_TOP : PL_FIELD_BOTTOM;
  }
  else
  {
    frameIn.field = PL_FIELD_NONE;
  }

  // --- Determine output viewport ---
  CRect src, dst, view;
  GetVideoRect(src, dst, view);
  int viewW = static_cast<int>(view.Width());
  int viewH = static_cast<int>(view.Height());
  if (viewW <= 0 || viewH <= 0)
    return false;

  // --- Wrap the current GL framebuffer as a libplacebo output texture ---
  GLint currentFbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &currentFbo);

  pl_opengl_wrap_params wrapParams{};
  wrapParams.framebuffer = static_cast<unsigned int>(currentFbo);
  wrapParams.width = viewW;
  wrapParams.height = viewH;
  wrapParams.iformat = GL_RGBA8;

  pl_tex outTex = pl_opengl_wrap(gpu, &wrapParams);
  if (!outTex)
  {
    CLog::Log(LOGERROR, "CRendererPLGLES::RenderHook - failed to wrap GL framebuffer");
    return false;
  }

  // --- Build output frame, cropped to the video destination rectangle ---
  pl_frame frameOut{};
  frameOut.num_planes = 1;
  frameOut.planes[0].texture = outTex;
  frameOut.planes[0].components = 3;
  frameOut.planes[0].component_mapping[0] = PL_CHANNEL_R;
  frameOut.planes[0].component_mapping[1] = PL_CHANNEL_G;
  frameOut.planes[0].component_mapping[2] = PL_CHANNEL_B;
  frameOut.planes[0].component_mapping[3] = PL_CHANNEL_NONE;
  frameOut.crop = {dst.x1, dst.y1, dst.x2, dst.y2};

  const CPictureBuffer& buf = m_buffers[idx];
  if (buf.m_srcColTransfer == AVCOL_TRC_SMPTEST2084 ||
      buf.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67)
  {
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
  frameOut.repr.levels =
      CServiceBroker::GetWinSystem()->UseLimitedColor() ? PL_COLOR_LEVELS_LIMITED
                                                        : PL_COLOR_LEVELS_FULL;

  pl_render_params params = m_plOpts->params;
  params.border = PL_CLEAR_SKIP;

  // Drain any GL errors accumulated during texture upload / EGLImage binding so
  // that libplacebo's gl_check_err("updating uniforms") in gl_pass_run does not
  // trigger an early return that skips scissor/blend/FBO cleanup (gpu_pass.c:563).
  while (glGetError() != GL_NO_ERROR)
    ;

  // Save GL state that libplacebo sets per-pass but may not restore on error paths:
  //   viewport — set every pass (gpu_pass.c:577), never explicitly restored
  //   scissor  — normally disabled at line 682, but skipped on early-return
  GLint savedViewport[4]{};
  glGetIntegerv(GL_VIEWPORT, savedViewport);
  GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
  GLint savedScissor[4]{};
  glGetIntegerv(GL_SCISSOR_BOX, savedScissor);
  // Save any named VAO that may be bound. libplacebo binds its own VAO per-pass
  // (gpu_pass.c:616) and resets to VAO 0 at line 675. Restoring the pre-render
  // binding ensures Kodi's subsequent draw calls find vertex attribute state in the
  // expected object (required by GLES 3.0 core and GL core profile).
  GLint savedVAO{};
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);

  if (!pl_render_image(renderer, &frameIn, &frameOut, &params))
    CLog::Log(LOGWARNING, "CRendererPLGLES::RenderHook - pl_render_image returned false");

  // Restore framebuffer: gl_tex_blit resets both bindings to 0 after every blit
  // (gpu_tex.c:869-870), so even a successful render leaves DRAW_FRAMEBUFFER=0.
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, currentFbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

  // Restore viewport and scissor unconditionally (no-op in the happy path where
  // libplacebo already cleaned up, but essential when gl_check_err aborts early).
  glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
  if (scissorWasEnabled)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  glScissor(savedScissor[0], savedScissor[1], savedScissor[2], savedScissor[3]);

  // Restore the previously bound VAO: libplacebo resets to VAO 0 at gpu_pass.c:675.
  // Required for GLES 3.0 core profile environments where VAO 0 is not a valid draw
  // target; draw calls with no named VAO bound generate GL_INVALID_OPERATION.
  glBindVertexArray(savedVAO);

  // Reset active texture unit to GL_TEXTURE0. Libplacebo binds textures to multiple
  // units during rendering and may leave the active unit at a non-zero index.
  glActiveTexture(GL_TEXTURE0);

  // Reset active shader program: libplacebo's gl_pass_run calls glUseProgram(pass->program)
  // at gpu_pass.c:555 but never restores it. Kodi's CGLShader tracks m_lastProgram and
  // skips glUseProgram if it believes its program is already bound — so if libplacebo's
  // program is still active, Kodi renders the GUI overlay with the wrong shader → invisible.
  glUseProgram(0);

  pl_tex_destroy(gpu, &outTex);

  // Always return true: we own this buffer slot and the base class must not
  // fall through to its own render path (which would crash on uninitialized GL planes).
  return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void CRendererPLGLES::ReleasePLBuffer(int index)
{
  PLBuffer& plbuf = m_plBuffers[index];

  // Always clean up VAAPI resources — partial failures in UploadTexture can leave
  // GL textures, EGLImages, and exported fds allocated even when loaded == false.
#if defined(HAVE_LIBVA)
  if (m_isVAAPI)
  {
    pl_gpu gpu = PL::PLInstance::Get()->GetGpu();
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

  // Unmap DRMPRIME EGLImage before destroying the pl_tex wrapper.
  m_drmTextures[index].Unmap();

  pl_gpu gpu = PL::PLInstance::Get()->GetGpu();
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
