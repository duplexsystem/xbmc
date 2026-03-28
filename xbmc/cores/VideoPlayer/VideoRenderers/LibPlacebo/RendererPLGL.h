/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "PlHelper.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/DRMPRIMEEGL.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGL.h"

#if defined(HAVE_LIBVA)
#include "system_egl.h"

#include <EGL/eglext.h>
#endif

extern "C"
{
#include <libavutil/pixfmt.h>
}

#include <array>

/**
 * @brief Linux libplacebo renderer using an OpenGL GPU context.
 *
 * Subclasses CLinuxRendererGL and intercepts the render hook to hand frames
 * off to libplacebo (pl_render_image) instead of Kodi's YUV shader pipeline.
 * Software-decoded frames are uploaded via pl_upload_plane().
 * VAAPI hardware-decoded frames are imported zero-copy via vaExportSurfaceHandle
 * (DRM PRIME 2 → per-plane DMA-buf fds → EGLImage → GL_TEXTURE_2D → pl_opengl_wrap).
 * Using GL_TEXTURE_2D (via GL_OES_EGL_image) avoids the samplerExternalOES restriction
 * in desktop GL 4.6 Core Profile GLSL shaders (Mesa Intel does not support it there).
 * After each pl_render_image call, GL_DRAW_FRAMEBUFFER is explicitly restored so that
 * libplacebo's failed-blit GL state does not corrupt Kodi's subsequent GUI rendering.
 * DRMPRIME frames use CDRMPRIMETexture (DMA-buf → EGLImage → GL_TEXTURE_EXTERNAL_OES)
 * then pl_opengl_wrap().
 */
class CRendererPLGL : public CLinuxRendererGL
{
public:
  CRendererPLGL();
  ~CRendererPLGL() override;

  static CBaseRenderer* Create(CVideoBuffer* buffer);
  static bool Register();

  // CBaseRenderer / CLinuxRendererGL overrides
  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation) override;
  bool ConfigChanged(const VideoPicture& picture) override;
  bool Supports(ERENDERFEATURE feature) const override;
  bool Supports(ESCALINGMETHOD method) const override;
  void AddVideoPicture(const VideoPicture& picture, int index) override;
  bool Flush(bool saveBuffers) override;

protected:
  // Texture lifecycle
  bool CreateTexture(int index) override;
  void DeleteTexture(int index) override;
  bool UploadTexture(int index) override;

  // Render hook — bypasses Kodi's YUV shader and calls pl_render_image
  bool LoadShadersHook() override;
  bool RenderHook(int idx) override;

  // Called each frame to respond to settings/picture changes
  void UpdateVideoFilter() override;

  EShaderFormat GetShaderFormat() override;

private:
  // Per-buffer libplacebo plane data. We keep one set per NUM_BUFFERS slot.
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
#if defined(HAVE_LIBVA)
    // VAAPI: GL textures wrapping per-plane EGLImages (not freed by pl_tex_destroy)
    GLuint vaapiGLTex[3]{};
    EGLImageKHR vaapiEGLImage[3]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    int vaapiExportedFd[4]{-1, -1, -1, -1}; ///< raw fds from vaExportSurfaceHandle (up to 4)
    int vaapiNumFds{0};
#endif
  };

  std::array<PLBuffer, NUM_BUFFERS> m_plBuffers{};

  // DRMPRIME DMA-buf → EGLImage → combined OES texture (one per buffer slot)
  std::array<CDRMPRIMETexture, NUM_BUFFERS> m_drmTextures{};
  bool m_isDRMPRIME{false};

#if defined(HAVE_LIBVA)
  bool m_isVAAPI{false};
  EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};
  PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR{nullptr};
  PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR{nullptr};
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES{nullptr};
  bool m_hasEGLModifiers{false};
#endif

  AVPixelFormat m_format{AV_PIX_FMT_NONE};
  pl_color_space m_colorSpace{};
  pl_chroma_location m_chromaLocation{PL_CHROMA_UNKNOWN};
  pl_options m_plOpts{nullptr}; ///< Owns all libplacebo render parameters

  struct QueuedFrameState
  {
    int bufferIndex;
    CRendererPLGL* renderer;
  };

  pl_queue m_plQueue{nullptr};
  double m_queuePtsOffset{0.0};
  bool m_queuePtsOffsetSet{false};

  static bool MapCallback(pl_gpu gpu,
                          pl_tex* tex,
                          const struct pl_source_frame* src,
                          struct pl_frame* out);
  static void UnmapCallback(pl_gpu gpu, struct pl_frame* frame, const struct pl_source_frame* src);
  static void DiscardCallback(const struct pl_source_frame* src);

  // Release all libplacebo textures (and unmap any HW texture) for a slot
  void ReleasePLBuffer(int index);
};
