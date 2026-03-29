/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PLHelper.h"

#include "ServiceBroker.h"
#include "filesystem/File.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#if defined(HAS_DX)
#include "rendering/dx/RenderContext.h"

#include <mfobjects.h>
#else
#include <EGL/egl.h>
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
  static std::shared_ptr<PLInstance> sPLResources(new PLInstance);
  return sPLResources;
}

PL::PLInstance::PLInstance()
  : m_plLog(nullptr),
#if defined(HAS_DX)
    m_plD3d11(nullptr),
    m_plSwapchain(nullptr),
#else
    m_plGl(nullptr),
#endif
    m_plGpu(nullptr),
    m_plRenderer(nullptr),
    CurrentPrim(0),
    Currenttransfer(0),
    CurrentMatrix(0)
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
#if defined(HAS_DX)
  //d3d device
  pl_d3d11_params d3d_param{};
  d3d_param.device = DX::DeviceResources::Get()->GetD3DDevice();
  d3d_param.adapter = DX::DeviceResources::Get()->GetAdapter();
  d3d_param.adapter_luid = DX::DeviceResources::Get()->GetAdapterDesc().AdapterLuid;
  d3d_param.allow_software = true;
  d3d_param.force_software = false;
  d3d_param.no_compute = false;
  d3d_param.debug = false;
  //libplacebo dont touch it if 0
  d3d_param.max_frame_latency = 0;
  //this was added to libplacebo to handle multi threaded rendering for kodi

  m_plD3d11 = pl_d3d11_create(m_plLog, &d3d_param);
  if (!m_plD3d11)
    return false;
  m_plGpu = m_plD3d11->gpu;
  //swap chain
  pl_d3d11_swapchain_params swapchain_param{};
  swapchain_param.swapchain = DX::DeviceResources::Get()->GetSwapChain();
  //everything else is not used
  m_plSwapchain = pl_d3d11_create_swapchain(m_plD3d11, &swapchain_param);
  if (!m_plSwapchain)
    return false;
#else
  pl_opengl_params gl_params = pl_opengl_default_params;
  gl_params.egl_display = eglGetCurrentDisplay();
  gl_params.egl_context = eglGetCurrentContext();
  m_plGl = pl_opengl_create(m_plLog, &gl_params);
  if (!m_plGl)
  {
    CLog::Log(LOGERROR, "PLInstance::Init - failed to create libplacebo OpenGL context");
    return false;
  }
  m_plGpu = m_plGl->gpu;
#endif

  m_plRenderer = pl_renderer_create(m_plLog, m_plGpu);
  m_isInitialized = true;
  return true;
}
void PL::PLInstance::Reset()
{
  if (m_isInitialized)
  {
    pl_renderer_destroy(&m_plRenderer);
#if defined(HAS_DX)
    pl_swapchain_destroy(&m_plSwapchain);
    pl_d3d11_destroy(&m_plD3d11);
#else
    if (m_plGl)
      pl_opengl_destroy(&m_plGl);
#endif
    pl_log_destroy(&m_plLog);
    m_isInitialized = false;
  }
}

void PL::PLInstance::LogCurrent()
{
  if (CurrentPrim == PL_COLOR_PRIM_COUNT)
    CurrentPrim = 0;
  if (CurrentMatrix == PL_COLOR_SYSTEM_COUNT)
    CurrentMatrix = 0;
  if (Currenttransfer == PL_COLOR_TRC_COUNT)
    Currenttransfer = 0;
  std::string sSys = pl_color_system_name((pl_color_system)CurrentMatrix);
  std::string sTrans = pl_color_transfer_name((pl_color_transfer)Currenttransfer);
  std::string sPrim = pl_color_primaries_name((pl_color_primaries)CurrentPrim);
  CLog::Log(LOGINFO, "LibPlaceboCurrent Color Settings: Primaries: {}", sPrim.c_str());
  CLog::Log(LOGINFO, "LibPlaceboCurrent Color Settings: Transfer: {}", sTrans.c_str());
  CLog::Log(LOGINFO, "LibPlaceboCurrent Color Settings: Matrix: {}", sSys.c_str());
}

const char* PL::PLInstance::pl_color_primaries_short_names[PL_COLOR_PRIM_COUNT] = {
    "Auto",
    "BT.601 NTSC",
    "BT.601 PAL",
    "BT.709",
    "BT.470 M",
    "EBU Tech.",
    "BT.2020",
    "Apple RGB",
    "Adobe RGB (1998)",
    "ProPhoto RGB (ROMM)",
    "CIE 1931 RGB primaries",
    "DCI-P3",
    "DCI-P3 with D65",
    "Panasonic V-Gamut",
    "Sony S-Gamut",
    "Traditional film primaries with Illuminant C",
    "ACES Primaries #0",
    "ACES Primaries #1"};

const char* PL::PLInstance::pl_color_primaries_short_name(pl_color_primaries prim)
{
  assert(prim >= 0 && prim < PL_COLOR_PRIM_COUNT);
  return pl_color_primaries_short_names[prim];
}

const char* pl_color_transfer_short_names[PL_COLOR_TRC_COUNT] = {
    "Auto", // PL_COLOR_TRC_UNKNOWN
    "BT.1886", // PL_COLOR_TRC_BT_1886
    "IEC 61966-2-4", // PL_COLOR_TRC_SRGB
    "Linear light content", // PL_COLOR_TRC_LINEAR
    "Pure power gamma 1.8", // PL_COLOR_TRC_GAMMA18
    "Pure power gamma 2.0", // PL_COLOR_TRC_GAMMA20
    "Pure power gamma 2.2", // PL_COLOR_TRC_GAMMA22
    "Pure power gamma 2.4", // PL_COLOR_TRC_GAMMA24
    "Pure power gamma 2.6", // PL_COLOR_TRC_GAMMA26
    "Pure power gamma 2.8", // PL_COLOR_TRC_GAMMA28
    "ProPhoto RGB (ROMM)", // PL_COLOR_TRC_PRO_PHOTO
    "Digital Cinema Distribution", // PL_COLOR_TRC_ST428
    "BT.2100 PQ", // PL_COLOR_TRC_PQ
    "BT.2100 HLG", // PL_COLOR_TRC_HLG
    "Panasonic V-Log", // PL_COLOR_TRC_V_LOG
    "Sony S-Log1", // PL_COLOR_TRC_S_LOG1
    "Sony S-Log2" // PL_COLOR_TRC_S_LOG2
};

const char* PL::PLInstance::pl_color_transfer_short_name(pl_color_transfer trc)
{
  assert(trc >= 0 && trc < PL_COLOR_TRC_COUNT);
  return pl_color_transfer_short_names[trc];
}

const char* pl_color_system_short_names[PL_COLOR_SYSTEM_COUNT] = {
    "Auto", // PL_COLOR_SYSTEM_UNKNOWN
    "BT.601 (SD)", // PL_COLOR_SYSTEM_BT_601
    "BT.709 (HD)", // PL_COLOR_SYSTEM_BT_709
    "SMPTE-240M", // PL_COLOR_SYSTEM_SMPTE_240M
    "BT.2020 N-C", // PL_COLOR_SYSTEM_BT_2020_NC
    "BT.2020 C", // PL_COLOR_SYSTEM_BT_2020_C
    "BT.2100 PQ", // PL_COLOR_SYSTEM_BT_2100_PQ
    "BT.2100 HLG", // PL_COLOR_SYSTEM_BT_2100_HLG
    "Dolby Vision", // PL_COLOR_SYSTEM_DOLBYVISION
    "YCgCo", // PL_COLOR_SYSTEM_YCGCO
    "RGB", // PL_COLOR_SYSTEM_RGB
    "XYZ" // PL_COLOR_SYSTEM_XYZ
};

const char* PL::PLInstance::pl_color_system_short_name(pl_color_system sys)
{
  assert(sys >= 0 && sys < PL_COLOR_SYSTEM_COUNT);
  return pl_color_system_short_names[sys];
}

#if defined(HAS_DX)
void PL::PLInstance::fill_d3d_format(pl_d3d_format* info, DXGI_FORMAT format)
{
  memset(info, 0, sizeof(pl_d3d_format));

  switch (format)
  {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      info->bits.color_depth = 10;
      info->bits.sample_depth = 10;
      info->bits.bit_shift = 0;
      info->planes[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
      info->component_mapping[0][0] = PL_CHANNEL_R;
      info->component_mapping[0][1] = PL_CHANNEL_G;
      info->component_mapping[0][2] = PL_CHANNEL_B;
      info->component_mapping[0][3] = PL_CHANNEL_A;
      info->components[0] = 4;
      info->width_div[0] = 1;
      info->height_div[0] = 1;
      info->num_planes = 1;
      strcpy(info->description, "rgba10");
      break;

    case DXGI_FORMAT_R8G8B8A8_UNORM:
      info->bits.color_depth = 8;
      info->bits.sample_depth = 8;
      info->bits.bit_shift = 0;
      info->planes[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      info->component_mapping[0][0] = PL_CHANNEL_R;
      info->component_mapping[0][1] = PL_CHANNEL_G;
      info->component_mapping[0][2] = PL_CHANNEL_B;
      info->component_mapping[0][3] = PL_CHANNEL_A;
      info->components[0] = 4;
      info->width_div[0] = 1;
      info->height_div[0] = 1;
      info->num_planes = 1;
      strcpy(info->description, "rgba");
      break;

    case DXGI_FORMAT_B8G8R8A8_UNORM:
      info->bits.color_depth = 8;
      info->bits.sample_depth = 8;
      info->bits.bit_shift = 0;
      info->planes[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
      info->component_mapping[0][0] = PL_CHANNEL_R;
      info->component_mapping[0][1] = PL_CHANNEL_G;
      info->component_mapping[0][2] = PL_CHANNEL_B;
      info->component_mapping[0][3] = PL_CHANNEL_A;
      info->components[0] = 4;
      info->width_div[0] = 1;
      info->height_div[0] = 1;
      info->num_planes = 1;
      strcpy(info->description, "bgra");
      break;

    case DXGI_FORMAT_NV12:
      info->bits.color_depth = 8;
      info->bits.sample_depth = 8;
      info->bits.bit_shift = 0;
      info->planes[0] = DXGI_FORMAT_R8_UNORM; // Y plane
      info->planes[1] = DXGI_FORMAT_R8G8_UNORM; // UV plane
      info->component_mapping[0][0] = PL_CHANNEL_Y;
      info->component_mapping[1][0] = PL_CHANNEL_U;
      info->component_mapping[1][1] = PL_CHANNEL_V;
      info->components[0] = 1;
      info->components[1] = 2;
      info->width_div[0] = 1; // full width
      info->height_div[0] = 1; // full height
      info->width_div[1] = 2; // half width
      info->height_div[1] = 2; // half height
      info->num_planes = 2;
      strcpy(info->description, "nv12");
      break;

    case DXGI_FORMAT_P010:
      info->bits.color_depth = 10;
      info->bits.sample_depth = 16;
      info->bits.bit_shift = 6;
      info->planes[0] = DXGI_FORMAT_R16_UNORM; // Y plane
      info->planes[1] = DXGI_FORMAT_R16G16_UNORM; // UV plane
      info->component_mapping[0][0] = PL_CHANNEL_Y;
      info->component_mapping[1][0] = PL_CHANNEL_U;
      info->component_mapping[1][1] = PL_CHANNEL_V;
      info->components[0] = 1;
      info->components[1] = 2;
      info->width_div[0] = 1;
      info->height_div[0] = 1;
      info->width_div[1] = 2;
      info->height_div[1] = 2;
      info->num_planes = 2;
      strcpy(info->description, "p010");
      break;

    case DXGI_FORMAT_P016:
      info->bits.color_depth = 16;
      info->bits.sample_depth = 16;
      info->bits.bit_shift = 0;
      info->planes[0] = DXGI_FORMAT_R16_UNORM;
      info->planes[1] = DXGI_FORMAT_R16G16_UNORM;
      info->component_mapping[0][0] = PL_CHANNEL_Y;
      info->component_mapping[1][0] = PL_CHANNEL_U;
      info->component_mapping[1][1] = PL_CHANNEL_V;
      info->components[0] = 1;
      info->components[1] = 2;
      info->width_div[0] = 1;
      info->height_div[0] = 1;
      info->width_div[1] = 2;
      info->height_div[1] = 2;
      info->num_planes = 2;
      strcpy(info->description, "p016");
      break;

    case DXGI_FORMAT_YUY2:
      info->bits.color_depth = 8;
      info->bits.sample_depth = 16; // packed 2 bytes per component pair
      info->bits.bit_shift = 0;
      info->planes[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // pseudo-plane
      info->component_mapping[0][0] = PL_CHANNEL_R;
      info->component_mapping[0][1] = PL_CHANNEL_G;
      info->component_mapping[0][2] = PL_CHANNEL_B;
      info->component_mapping[0][3] = PL_CHANNEL_A;
      info->width_div[0] = 1;
      info->height_div[0] = 1;
      info->num_planes = 1;
      strcpy(info->description, "yuy2");
      break;

    default:
      info->num_planes = 0;
      strcpy(info->description, "unknown");
      break;
  }
}
#endif // HAS_DX

/*Settings conversion*/
const pl_tone_map_function* PL::PLInstance::GetToneMappingFunction(pl_tone_mapping method)
{
  switch (method)
  {
    case TONE_MAPPING_AUTO:
      return &pl_tone_map_auto;
    case TONE_MAPPING_CLIP:
      return &pl_tone_map_clip;
    case TONE_MAPPING_MOBIUS:
      return &pl_tone_map_mobius;
    case TONE_MAPPING_REINHARD:
      return &pl_tone_map_reinhard;
    case TONE_MAPPING_HABLE:
      return &pl_tone_map_hable;
    case TONE_MAPPING_GAMMA:
      return &pl_tone_map_gamma;
    case TONE_MAPPING_LINEAR:
      return &pl_tone_map_linear;
    case TONE_MAPPING_SPLINE:
      return &pl_tone_map_spline;
    case TONE_MAPPING_BT_2390:
      return &pl_tone_map_bt2390;
    case TONE_MAPPING_BT_2446A:
      return &pl_tone_map_bt2446a;
    case TONE_MAPPING_ST2094_40:
      return &pl_tone_map_st2094_40;
    case TONE_MAPPING_ST2094_10:
      return &pl_tone_map_st2094_10;
    default:
      return nullptr;
  }
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

void PL::RenderConfig::UpdateVideoFilter(ESCALINGMETHOD scalingMethod, const CVideoSettings& videoSettings)
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
                     settings->GetBool(CSettings::SETTING_VIDEOPLAYER_LIBPLACEBO_DEBAND) ? "yes" : "no");
  pl_options_set_str(m_plOpts, "peak_detect",
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
  if (!m_plCmsManager->GetVideo3dLut(srcPrimaries, &m_plCmsToken, CMS_DATA_FMT_RGB,
                                     clutSize, rawData.data()))
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
  CLog::Log(LOGDEBUG, "PL::RenderConfig::UpdateCmsLut - loaded {}³ CMS LUT (token {})", clutSize, m_plCmsToken);
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
  CLog::Log(LOGDEBUG, "PL::RenderConfig::UpdateIccProfile - opened ICC profile ({} bytes): {}", size, path);
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