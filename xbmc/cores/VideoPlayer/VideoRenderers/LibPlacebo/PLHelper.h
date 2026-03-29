/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "libplacebo/colorspace.h"
#if defined(HAS_DX)
#include "libplacebo/d3d11.h"
#else
#include "libplacebo/opengl.h"
#endif
#include "libplacebo/log.h"
#include "libplacebo/renderer.h"
#include "libplacebo/utils/frame_queue.h"
#include "libplacebo/utils/upload.h"

#include <libplacebo/options.h>
#include <libplacebo/shaders/icc.h>

#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"
#include "cores/VideoPlayer/VideoRenderers/ColorManager.h"
extern "C"
{
#include <libavutil/dovi_meta.h>
#include <libavutil/pixfmt.h>
}

#include <memory>
#include <string>
#include <vector>

#if defined(HAS_DX)
#include <d3d9types.h>
#include <dxva2api.h>
#endif
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>
#if defined(HAS_DX)
#include <strmif.h>
#endif

#define MAX_FRAME_PASSES 256
#define MAX_BLEND_PASSES 8
#define MAX_BLEND_FRAMES 8
namespace PL
{
#if defined(HAS_DX)
typedef struct pl_d3d_format
{
  pl_bit_encoding bits; // per picture
  DXGI_FORMAT planes[4]; // DXGI format per plane
  int components[4]; // number of components per plane
  pl_channel component_mapping[4][4];
  int width_div[4]; // divide full width by this for each plane
  int height_div[4]; // divide full height by this for each plane
  char description[16]; // short description
  int num_planes; // actual number of planes used
} pl_d3d_format;
#endif

enum pl_tone_mapping
{
  TONE_MAPPING_AUTO,
  TONE_MAPPING_CLIP,
  TONE_MAPPING_MOBIUS,
  TONE_MAPPING_REINHARD,
  TONE_MAPPING_HABLE,
  TONE_MAPPING_GAMMA,
  TONE_MAPPING_LINEAR,
  TONE_MAPPING_SPLINE,
  TONE_MAPPING_BT_2390,
  TONE_MAPPING_BT_2446A,
  TONE_MAPPING_ST2094_40,
  TONE_MAPPING_ST2094_10,
};

enum pl_gamut_mode
{
  GAMUT_AUTO,
  GAMUT_CLIP,
  GAMUT_PERCEPTUAL,
  GAMUT_RELATIVE,
  GAMUT_SATURATION,
  GAMUT_ABSOLUTE,
  GAMUT_DESATURATE,
  GAMUT_DARKEN,
  GAMUT_WARN,
  GAMUT_LINEAR,
};

class PLInstance
{
public:
  static std::shared_ptr<PLInstance> Get();
  PLInstance();

  virtual ~PLInstance();
  bool Init();
  void Reset();

#if defined(HAS_DX)
  pl_d3d11 GetD3d11() { return m_plD3d11; }
  pl_swapchain GetSwapchain() { return m_plSwapchain; }
#else
  pl_opengl GetOpenGL() { return m_plGl; }
#endif
  pl_renderer GetRenderer() { return m_plRenderer; }
  pl_gpu GetGpu() { return m_plGpu; }

  pl_log m_plLog;
#if defined(HAS_DX)
  pl_d3d11 m_plD3d11;
  pl_swapchain m_plSwapchain;
#else
  pl_opengl m_plGl;
#endif
  pl_gpu m_plGpu;
  pl_renderer m_plRenderer;
  int CurrentPrim;
  int Currenttransfer;
  int CurrentMatrix;
  void LogCurrent();

  static const char* pl_color_primaries_short_names[PL_COLOR_PRIM_COUNT];
  static const char* pl_color_primaries_short_name(pl_color_primaries prim);
  static const char* pl_color_transfer_shorts_name[PL_COLOR_TRC_COUNT];
  static const char* pl_color_transfer_short_name(pl_color_transfer trc);
  static const char* pl_color_system_shorts_name[PL_COLOR_SYSTEM_COUNT];
  static const char* pl_color_system_short_name(pl_color_system sys);

#if defined(HAS_DX)
  void fill_d3d_format(pl_d3d_format* info, DXGI_FORMAT format);
#endif

  const pl_tone_map_function* GetToneMappingFunction(pl_tone_mapping method);

private:
  bool m_isInitialized{false};
};

constexpr pl_rotation RotationFromOrientation(unsigned int deg)
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

const char* KodiScalingToPlacebo(ESCALINGMETHOD method);

template<typename TBuffer>
void ApplyHdrMetadata(pl_color_space& colorSpace, pl_color_repr& colorRepr, pl_dovi_metadata& doviMetadata, const TBuffer& b)
{
  if (b.m_srcColTransfer == AVCOL_TRC_SMPTEST2084 || b.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67)
  {
    pl_hdr_metadata& hdr = colorSpace.hdr;
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
      colorSpace = b.plColorSpace;
      colorRepr = b.plColorRepr;
      doviMetadata = b.plDoviMetadata;
      colorRepr.dovi = &doviMetadata;
    }
    else
    {
      pl_hdr_metadata_merge(&colorSpace.hdr, &b.plColorSpace.hdr);
    }
  }
}

class RenderConfig
{
public:
  RenderConfig();
  ~RenderConfig();

  // Applies Kodi GUI video settings (brightness, quality, algorithms)
  void UpdateVideoFilter(ESCALINGMETHOD scalingMethod, const CVideoSettings& videoSettings);

  // Reloads CMS LUT / ICC profile based on settings
  void ApplyCMS(pl_frame& frameOut, AVColorPrimaries srcPrimaries);

  // Flushes state when a new video is played
  void ResetCmsState();

  pl_options GetOptions() const { return m_plOpts; }

private:
  void UpdateCmsLut(AVColorPrimaries srcPrimaries);
  void UpdateIccProfile();

  pl_options m_plOpts{nullptr};

  std::unique_ptr<CColorManager> m_plCmsManager;
  pl_icc_object m_iccObject{nullptr};
  std::vector<uint8_t> m_iccData;
  std::string m_iccPath;
  uint64_t m_iccSignature{0};

  std::vector<float> m_cmsLutData;
  pl_custom_lut m_cmsLut{};
  int m_plCmsToken{-1};
  bool m_cmsLutValid{false};
};

} // namespace PL
