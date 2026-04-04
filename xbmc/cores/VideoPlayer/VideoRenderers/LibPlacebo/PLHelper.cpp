/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PLHelper.h"

#include "ServiceBroker.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#if defined(HAS_GL) || defined(HAS_GLES)
#include "windowing/WinSystem.h"
#include "windowing/linux/WinSystemEGL.h"

#include <EGL/egl.h>

#include <memory>
#endif

static void pl_log_cb(void*, enum pl_log_level level, const char* msg)
{
  switch (level)
  {
    case PL_LOG_FATAL:
      CLog::Log(LOGFATAL, "libPlacebo Fatal: {}", msg);
      break;
    case PL_LOG_ERR:
      CLog::Log(LOGERROR, "libPlacebo Error: {}", msg);

      break;
    case PL_LOG_WARN:
      CLog::Log(LOGWARNING, "libPlacebo Warning: {}", msg);
      break;
    case PL_LOG_INFO:
      CLog::Log(LOGINFO, "libPlacebo Info: {}", msg);
      break;
    case PL_LOG_DEBUG:
      CLog::Log(LOGDEBUG, "libPlacebo Debug: {}", msg);
      break;
    case PL_LOG_NONE:
    case PL_LOG_TRACE:
      CLog::Log(LOGNONE, "libPlacebo Trace: {}", msg);
      break;
  }
}

std::shared_ptr<PL::PLInstance> PL::PLInstance::Get()
{
  static std::shared_ptr<PLInstance> sPLResources = std::make_shared<PLInstance>();
  return sPLResources;
}

PL::PLInstance::PLInstance()
  : m_plLog(nullptr),
#if defined(HAS_GL) || defined(HAS_GLES)
    m_plGl(nullptr),
#endif
    m_plGpu(nullptr),
    m_plRenderer(nullptr),
    m_plCache(nullptr)
{
}

PL::PLInstance::~PLInstance() = default;

bool PL::PLInstance::Init()
{
  if (m_isInitialized)
    return true;

  pl_log_params log_param{};
  log_param.log_cb = pl_log_cb;
  log_param.log_level = PL_LOG_DEBUG;
  m_plLog = pl_log_create(PL_API_VER, &log_param);
#if defined(HAS_GL) || defined(HAS_GLES)
  // On GBM (RPi5), eglGetCurrentDisplay/Context() can return EGL_NO_*
  // because the platform display is obtained via eglGetPlatformDisplay()
  // and with DRMPRIME direct-to-plane the GLES context may never be made
  // current on this thread.  Pull display and context from the windowing
  // system directly, falling back to the EGL thread-local getters.
  EGLDisplay eglDpy = EGL_NO_DISPLAY;
  EGLContext eglCtx = EGL_NO_CONTEXT;
  auto* winEGL =
      dynamic_cast<KODI::WINDOWING::LINUX::CWinSystemEGL*>(CServiceBroker::GetWinSystem());
  if (winEGL)
  {
    eglDpy = winEGL->GetEGLDisplay();
    eglCtx = winEGL->GetEGLContext();
  }
  if (eglDpy == EGL_NO_DISPLAY)
    eglDpy = eglGetCurrentDisplay();
  if (eglCtx == EGL_NO_CONTEXT)
    eglCtx = eglGetCurrentContext();

  // If the context is not current on this thread, make it current now.
  // libplacebo calls eglGetCurrentContext() internally even when egl_context
  // is provided in params, so the context must be bound to the calling thread.
  // This is safe: on GBM the context is surfaceless and not held by any
  // thread between render frames, so eglMakeCurrent will succeed.
  const bool needMakeCurrent =
      (eglCtx != EGL_NO_CONTEXT && eglGetCurrentContext() != eglCtx);
  if (needMakeCurrent)
  {
    if (!eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, eglCtx))
      CLog::Log(LOGWARNING, "PLInstance::Init - eglMakeCurrent failed: 0x{:x}", eglGetError());
  }

  pl_opengl_params gl_params = pl_opengl_default_params;
  gl_params.egl_display = eglDpy;
  gl_params.egl_context = eglCtx;
  // Explicitly provide eglGetProcAddress so libplacebo uses EGL's function
  // loader rather than its own internal logic (which may fail to load core
  // GLES functions on some platforms, e.g. Broadcom V3D on RPi5).
  gl_params.get_proc_addr =
      reinterpret_cast<pl_voidfunc_t (*)(const char*)>(eglGetProcAddress);
  m_plGl = pl_opengl_create(m_plLog, &gl_params);
  if (!m_plGl)
  {
    CLog::Log(LOGERROR, "PLInstance::Init - failed to create libplacebo OpenGL context");
    return false;
  }
  m_plGpu = m_plGl->gpu;
#else
  CLog::Log(LOGERROR, "PLInstance::Init - no libplacebo backend enabled");
  return false;
#endif

  // Create shader cache, load any previously-saved entries, then attach to the
  // GPU before the renderer is created so shader compilation can be cached.
  pl_cache_params cacheParams{};
  cacheParams.log = m_plLog;
  cacheParams.max_total_size = 128 * 1024 * 1024; // 128 MB cap (matches mpv)
  m_plCache = pl_cache_create(&cacheParams);
  LoadCache();
  pl_gpu_set_cache(m_plGpu, m_plCache);

  m_plRenderer = pl_renderer_create(m_plLog, m_plGpu);
  m_isInitialized = true;
  return true;
}

void PL::PLInstance::Reset()
{
  if (m_isInitialized)
  {
    pl_renderer_destroy(&m_plRenderer);
    SaveCache();
    pl_cache_destroy(&m_plCache);
#if defined(HAS_GL) || defined(HAS_GLES)
    if (m_plGl)
      pl_opengl_destroy(&m_plGl);
#endif
    pl_log_destroy(&m_plLog);
    m_isInitialized = false;
  }
}

static constexpr const char* kCacheDir = "special://profile/libplacebo/";
static constexpr const char* kCachePath = "special://profile/libplacebo/shadercache.bin";

void PL::PLInstance::LoadCache()
{
  XFILE::CFile f;
  if (!f.Open(kCachePath))
    return;

  const int loaded = pl_cache_load_ex(
      m_plCache,
      [](void* priv, size_t size, void* ptr) -> bool {
        return static_cast<XFILE::CFile*>(priv)->Read(ptr, size) ==
               static_cast<ssize_t>(size);
      },
      &f);
  f.Close();

  if (loaded >= 0)
  {
    m_cacheSignature = pl_cache_signature(m_plCache);
    CLog::Log(LOGDEBUG, "PLInstance::LoadCache - loaded {} shader objects ({} bytes)", loaded,
              pl_cache_size(m_plCache));
  }
  else
  {
    CLog::Log(LOGWARNING, "PLInstance::LoadCache - cache file corrupt, ignoring");
  }
}

void PL::PLInstance::SaveCache()
{
  if (!m_plCache || pl_cache_signature(m_plCache) == m_cacheSignature)
    return; // nothing new to persist

  if (!XFILE::CDirectory::Exists(kCacheDir))
    XFILE::CDirectory::Create(kCacheDir);

  XFILE::CFile f;
  if (!f.OpenForWrite(kCachePath, true))
  {
    CLog::Log(LOGERROR, "PLInstance::SaveCache - failed to open cache file for writing");
    return;
  }

  pl_cache_save_ex(
      m_plCache,
      [](void* priv, size_t size, const void* ptr) {
        static_cast<XFILE::CFile*>(priv)->Write(ptr, size);
      },
      &f);
  f.Close();

  CLog::Log(LOGDEBUG, "PLInstance::SaveCache - saved {} shader objects ({} bytes)",
            pl_cache_objects(m_plCache), pl_cache_size(m_plCache));
}

const char* PL::KodiScalingToPlacebo(ESCALINGMETHOD method)
{
  switch (method)
  {
    case VS_SCALINGMETHOD_NEAREST:
      return "nearest";
    case VS_SCALINGMETHOD_LINEAR:
      return "bilinear";
    case VS_SCALINGMETHOD_CUBIC_B_SPLINE:
      return "bicubic";
    case VS_SCALINGMETHOD_CUBIC_MITCHELL:
      return "mitchell";
    case VS_SCALINGMETHOD_CUBIC_CATMULL:
      return "catmull_rom";
    case VS_SCALINGMETHOD_LANCZOS2:
    case VS_SCALINGMETHOD_LANCZOS3_FAST:
    case VS_SCALINGMETHOD_LANCZOS3:
      return "lanczos";
    case VS_SCALINGMETHOD_SPLINE36_FAST:
    case VS_SCALINGMETHOD_SPLINE36:
      return "spline36";
    default:
      return nullptr;
  }
}

PL::RenderConfig::RenderConfig()
{
  m_plOpts = pl_options_alloc(PL::PLInstance::Get()->m_plLog);
  m_plCmsManager = std::make_unique<CColorManager>();
}

PL::RenderConfig::~RenderConfig()
{
  pl_icc_close(&m_iccObject);
  pl_options_free(&m_plOpts);
}

void PL::RenderConfig::ResetCmsState()
{
  m_plCmsToken = -1;
  m_cmsLutValid = false;
  pl_icc_close(&m_iccObject);
  m_iccPath.clear();
  m_iccData.clear();
  m_iccSignature = 0;
}

void PL::RenderConfig::UpdateVideoFilter(ESCALINGMETHOD scalingMethod,
                                         const CVideoSettings& videoSettings) const
{
  pl_options_reset(m_plOpts, nullptr);

  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  {
    static constexpr const char* kPresets[] = {"fast", "default", "high_quality"};
    const int preset = settings->GetInt(CSettings::SETTING_VIDEOPLAYER_LIBPLACEBO_PRESET);
    if (preset >= 0 && preset < static_cast<int>(std::size(kPresets)))
      pl_options_set_str(m_plOpts, "preset", kPresets[preset]);
  }

  pl_options_set_str(m_plOpts, "deband",
                     settings->GetBool(CSettings::SETTING_VIDEOPLAYER_LIBPLACEBO_DEBAND) ? "yes"
                                                                                         : "no");
  pl_options_set_str(
      m_plOpts, "peak_detect",
      settings->GetBool(CSettings::SETTING_VIDEOPLAYER_LIBPLACEBO_PEAKDETECT) ? "yes" : "no");
  if (settings->GetBool(CSettings::SETTING_VIDEOPLAYER_LIBPLACEBO_FRAMEMIX))
    pl_options_set_str(m_plOpts, "frame_mixer", "oversample");

  if (const char* filter = PL::KodiScalingToPlacebo(scalingMethod))
  {
    pl_options_set_str(m_plOpts, "upscaler", filter);
    pl_options_set_str(m_plOpts, "downscaler", filter);
  }

  static constexpr const char* kToneMaps[] = {nullptr, "reinhard", "spline", "hable"};
  if (videoSettings.m_ToneMapMethod > 0 && videoSettings.m_ToneMapMethod < VS_TONEMAPMETHOD_MAX)
    pl_options_set_str(m_plOpts, "tone_mapping", kToneMaps[videoSettings.m_ToneMapMethod]);

  pl_options_set_str(m_plOpts, "dither",
                     settings->GetBool(CSettings::SETTING_VIDEOSCREEN_DITHER) ? "yes" : "no");

  switch (videoSettings.m_InterlaceMethod)
  {
    case VS_INTERLACEMETHOD_NONE:
      pl_options_set_str(m_plOpts, "deinterlace", "no");
      break;
    case VS_INTERLACEMETHOD_LIBPLACEBO_BOB:
      pl_options_set_str(m_plOpts, "deinterlace", "yes");
      pl_options_set_str(m_plOpts, "deinterlace_algo", "bob");
      break;
    case VS_INTERLACEMETHOD_LIBPLACEBO_YADIF:
      pl_options_set_str(m_plOpts, "deinterlace", "yes");
      pl_options_set_str(m_plOpts, "deinterlace_algo", "yadif");
      break;
    case VS_INTERLACEMETHOD_LIBPLACEBO_BWDIF:
      pl_options_set_str(m_plOpts, "deinterlace", "yes");
      pl_options_set_str(m_plOpts, "deinterlace_algo", "bwdif");
      break;
    default:
      break;
  }

  {
    const float brightness = (videoSettings.m_Brightness - 50.0f) / 50.0f;
    const float contrast = videoSettings.m_Contrast / 50.0f;
    if (brightness != 0.0f || contrast != 1.0f)
    {
      // Initialise to neutral first so saturation/gamma/hue/temperature stay
      // at their identity values and only the user-controlled fields are changed.
      m_plOpts->color_adjustment = pl_color_adjustment_neutral;
      m_plOpts->color_adjustment.brightness = brightness;
      m_plOpts->color_adjustment.contrast = contrast;
      m_plOpts->params.color_adjustment = &m_plOpts->color_adjustment;
    }
  }
}

void PL::RenderConfig::UpdateCmsLut(AVColorPrimaries srcPrimaries)
{
  if (!m_plCmsManager->IsEnabled() || !m_plCmsManager->IsValid())
  {
    m_cmsLutValid = false;
    return;
  }

  if (m_plCmsManager->CheckConfiguration(m_plCmsToken, srcPrimaries))
    return;

  int clutSize = 0;
  int dataSize = 0;
  if (!CColorManager::Get3dLutSize(CMS_DATA_FMT_RGB, &clutSize, &dataSize))
  {
    CLog::Log(LOGERROR, "PL::RenderConfig::UpdateCmsLut - Get3dLutSize failed");
    m_cmsLutValid = false;
    return;
  }

  std::vector<uint16_t> rawData(dataSize / sizeof(uint16_t));
  if (!m_plCmsManager->GetVideo3dLut(srcPrimaries, &m_plCmsToken, CMS_DATA_FMT_RGB, clutSize,
                                     rawData.data()))
  {
    CLog::Log(LOGERROR, "PL::RenderConfig::UpdateCmsLut - GetVideo3dLut failed");
    m_cmsLutValid = false;
    return;
  }

  m_cmsLutData.resize(rawData.size());
  constexpr float kScale = 1.0f / 65535.0f;
  for (size_t i = 0; i < rawData.size(); ++i)
    m_cmsLutData[i] = static_cast<float>(rawData[i]) * kScale;

  m_cmsLut = pl_custom_lut{};
  m_cmsLut.size[0] = clutSize;
  m_cmsLut.size[1] = clutSize;
  m_cmsLut.size[2] = clutSize;
  m_cmsLut.data = m_cmsLutData.data();
  m_cmsLut.signature = static_cast<uint64_t>(static_cast<unsigned int>(m_plCmsToken));

  m_cmsLutValid = true;
  CLog::Log(LOGDEBUG, "PL::RenderConfig::UpdateCmsLut - loaded {}³ CMS LUT (token {})", clutSize,
            m_plCmsToken);
}

void PL::RenderConfig::UpdateIccProfile()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const std::string path = settings->GetString(CSettings::SETTING_VIDEOSCREEN_DISPLAYPROFILE);

  if (path == m_iccPath)
    return;

  m_iccData.clear();
  m_iccPath.clear();
  m_iccSignature = 0;

  if (path.empty())
    return;

  XFILE::CFile f;
  if (!f.Open(path))
  {
    CLog::Log(LOGERROR, "PL::RenderConfig::UpdateIccProfile - cannot open ICC file: {}", path);
    return;
  }

  const int64_t size = f.GetLength();
  if (size <= 0)
  {
    CLog::Log(LOGERROR, "PL::RenderConfig::UpdateIccProfile - empty ICC file: {}", path);
    f.Close();
    return;
  }

  m_iccData.resize(static_cast<size_t>(size));
  f.Read(m_iccData.data(), size);
  f.Close();

  pl_icc_profile iccProf{};
  iccProf.data = m_iccData.data();
  iccProf.len = m_iccData.size();
  pl_icc_profile_compute_signature(&iccProf);
  m_iccSignature = iccProf.signature;

  if (!pl_icc_update(PL::PLInstance::Get()->m_plLog, &m_iccObject, &iccProf, nullptr))
  {
    CLog::Log(LOGERROR, "PL::RenderConfig::UpdateIccProfile - pl_icc_update failed: {}", path);
    m_iccData.clear();
    m_iccSignature = 0;
    return;
  }

  m_iccPath = path;
  CLog::Log(LOGDEBUG, "PL::RenderConfig::UpdateIccProfile - opened ICC profile ({} bytes): {}",
            size, path);
}

void PL::RenderConfig::ApplyCMS(pl_frame& frameOut, AVColorPrimaries srcPrimaries)
{
  const auto cmsSettings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const bool cmsEnabled = cmsSettings->GetBool("videoscreen.cmsenabled");
  const int cmsMode = cmsSettings->GetInt("videoscreen.cmsmode");
  if (cmsEnabled && cmsMode == CMS_MODE_PROFILE)
  {
    UpdateIccProfile();
    if (m_iccObject)
      frameOut.icc = m_iccObject;
  }
  else
  {
    UpdateCmsLut(srcPrimaries);
    if (m_cmsLutValid)
    {
      frameOut.lut = &m_cmsLut;
      frameOut.lut_type = PL_LUT_NATIVE;
    }
  }
}