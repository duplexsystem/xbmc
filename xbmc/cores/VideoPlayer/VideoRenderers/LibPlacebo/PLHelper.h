/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "libplacebo/colorspace.h"
#if defined(HAS_GL) || defined(HAS_GLES)
#include "libplacebo/opengl.h"
#endif
#include "cores/VideoPlayer/VideoRenderers/ColorManager.h"
#include "cores/VideoPlayer/VideoRenderers/RenderInfo.h"
#include "libplacebo/log.h"
#include "libplacebo/renderer.h"
#include "libplacebo/utils/frame_queue.h"
#include "libplacebo/utils/upload.h"

#include <libplacebo/options.h>
#include <libplacebo/shaders/icc.h>
extern "C"
{
#include <libavutil/dovi_meta.h>
#include <libavutil/pixfmt.h>
}

#include <memory>
#include <string>
#include <vector>

#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>

#define MAX_FRAME_PASSES 256
#define MAX_BLEND_PASSES 8
#define MAX_BLEND_FRAMES 8
namespace PL
{

class PLInstance
{
public:
  static std::shared_ptr<PLInstance> Get();
  PLInstance();

  virtual ~PLInstance();
  bool Init();
  void Reset();

#if defined(HAS_GL) || defined(HAS_GLES)
  pl_opengl GetOpenGL() { return m_plGl; }
#endif
  pl_renderer GetRenderer() { return m_plRenderer; }
  pl_gpu GetGpu() { return m_plGpu; }

  pl_log m_plLog;
#if defined(HAS_GL) || defined(HAS_GLES)
  pl_opengl m_plGl;
#endif
  pl_gpu m_plGpu;
  pl_renderer m_plRenderer;

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
void ApplyHdrMetadata(pl_color_space& colorSpace,
                      pl_color_repr& colorRepr,
                      pl_dovi_metadata& doviMetadata,
                      const TBuffer& b)
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
  void UpdateVideoFilter(ESCALINGMETHOD scalingMethod, const CVideoSettings& videoSettings) const;

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
