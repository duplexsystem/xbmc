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

#include <libplacebo/cache.h>
#include <libplacebo/gpu.h>
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
  pl_cache m_plCache;

private:
  void LoadCache();
  void SaveCache();

  bool m_isInitialized{false};
  uint64_t m_cacheSignature{0};
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

// Merge HDR metadata into colorSpace from the decoded buffer.
//
// Priority 1: Dolby Vision — decoder provides complete, authoritative metadata
// via pl_map_avdovi_metadata. Use it directly; no fallbacks needed.
//
// Priority 2: Decoder's pl_map_hdr_metadata result (frame side data: mastering
// display + content light + HDR10+ dynamic metadata). This is the authoritative
// source for per-frame HDR metadata.
//
// Priority 3 (fallback): When pl_map_hdr_metadata got no frame side data but
// the stream is HDR, manually extract from displayMetadata/lightMetadata (which
// come from stream hints via DVDVideoCodecFFmpeg's m_hints fallback path).
template<typename TBuffer>
void ApplyHdrMetadata(pl_color_space& colorSpace,
                      pl_color_repr& colorRepr,
                      pl_dovi_metadata& doviMetadata,
                      const TBuffer& b)
{
  // Priority 1: Dolby Vision — early return with authoritative metadata
  if (b.plColorRepr.dovi != nullptr)
  {
    colorSpace = b.plColorSpace;
    colorRepr = b.plColorRepr;
    doviMetadata = b.plDoviMetadata;
    colorRepr.dovi = &doviMetadata;
    return;
  }

  // Priority 2: Decoder's pl_map_hdr_metadata result (per-frame side data)
  if (!pl_hdr_metadata_equal(&b.plColorSpace.hdr, &pl_hdr_metadata_empty))
  {
    pl_hdr_metadata_merge(&colorSpace.hdr, &b.plColorSpace.hdr);
  }
  else if (b.m_srcColTransfer == AVCOL_TRC_SMPTEST2084 ||
           b.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67)
  {
    // Priority 3: Fallback — pl_map_hdr_metadata got no frame side data, but
    // stream hints provided metadata via DVDVideoCodecFFmpeg's m_hints path.
    if (b.hasDisplayMetadata)
    {
      const auto& m = b.displayMetadata;
      auto& hdr = colorSpace.hdr;
      hdr.prim.red = {av_q2d(m.display_primaries[0][0]), av_q2d(m.display_primaries[0][1])};
      hdr.prim.green = {av_q2d(m.display_primaries[1][0]), av_q2d(m.display_primaries[1][1])};
      hdr.prim.blue = {av_q2d(m.display_primaries[2][0]), av_q2d(m.display_primaries[2][1])};
      hdr.prim.white = {av_q2d(m.white_point[0]), av_q2d(m.white_point[1])};
      if (m.has_luminance)
      {
        hdr.max_luma = av_q2d(m.max_luminance);
        hdr.min_luma = av_q2d(m.min_luminance);
      }
    }
    if (b.hasLightMetadata)
    {
      colorSpace.hdr.max_cll = b.lightMetadata.MaxCLL;
      colorSpace.hdr.max_fall = b.lightMetadata.MaxFALL;
    }
  }
}

// Ensures primaries, transfer, and YCbCr system are valid for libplacebo.
// Uses HDR metadata presence (max_luma, max_cll) to make informed defaults
// instead of blind guessing. Also guards against out-of-range enum values
// that cause SIGSEGV via __builtin_unreachable() on ARM64.
inline void ValidateInputColorSpace(pl_color_space& colorSpace, pl_color_repr& colorRepr)
{
  const bool hasHdrLuminance = colorSpace.hdr.max_luma > 0 || colorSpace.hdr.max_cll > 0;
  const bool hasHdrSceneInfo = colorSpace.hdr.scene_max[0] > 0;
  const bool isHdrContent = hasHdrLuminance || hasHdrSceneInfo;

  // Primaries: use metadata-informed default instead of blind BT.709
  if (colorSpace.primaries <= PL_COLOR_PRIM_UNKNOWN ||
      colorSpace.primaries >= PL_COLOR_PRIM_COUNT)
    colorSpace.primaries = isHdrContent ? PL_COLOR_PRIM_BT_2020 : PL_COLOR_PRIM_BT_709;

  // Transfer: use metadata-informed default instead of blind BT.1886
  if (colorSpace.transfer <= PL_COLOR_TRC_UNKNOWN || colorSpace.transfer >= PL_COLOR_TRC_COUNT)
    colorSpace.transfer = isHdrContent ? PL_COLOR_TRC_PQ : PL_COLOR_TRC_BT_1886;

  // HDR transfer implies BT.2020 primaries when confirmed by metadata
  if ((colorSpace.transfer == PL_COLOR_TRC_PQ || colorSpace.transfer == PL_COLOR_TRC_HLG) &&
      colorSpace.primaries == PL_COLOR_PRIM_BT_709 && isHdrContent)
  {
    colorSpace.primaries = PL_COLOR_PRIM_BT_2020;
  }

  // HDR YCbCr content must use BT.2020 matrix
  if ((colorSpace.transfer == PL_COLOR_TRC_PQ || colorSpace.transfer == PL_COLOR_TRC_HLG) &&
      pl_color_system_is_ycbcr_like(colorRepr.sys) &&
      colorRepr.sys != PL_COLOR_SYSTEM_BT_2020_NC &&
      colorRepr.sys != PL_COLOR_SYSTEM_BT_2020_C &&
      colorRepr.sys != PL_COLOR_SYSTEM_BT_2100_PQ &&
      colorRepr.sys != PL_COLOR_SYSTEM_BT_2100_HLG)
  {
    colorRepr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
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
