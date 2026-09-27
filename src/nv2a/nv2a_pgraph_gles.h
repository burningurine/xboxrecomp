/*
 * nv2a_pgraph_gles.h - GLES 3.0 rasteriser for NV2A object-space batches.
 *
 * See nv2a_pgraph_gles.c for the design. Producer functions are called from the
 * pushbuffer parser thread (kernel/nv2a_pb_exec.c); nv2a_gles_render() is called
 * from the presenter GL thread (video/fb_present.c) with the EGL context current.
 * Gated at runtime by RECOMP_GLES_3D; all functions are safe no-ops when off.
 */
#ifndef NV2A_PGRAPH_GLES_H
#define NV2A_PGRAPH_GLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if RECOMP_GLES_3D is set (the GLES 3D path is active), else 0. */
int  nv2a_gles_enabled(void);

/* Producer (parser thread). Record the batch's clear colour (ARGB8888). */
void nv2a_gles_clear(uint32_t argb);

/* Producer (parser thread). One triangle in CLIP space: each cN = {x,y,z,w}
 * as produced by the software transform; argb is the flat vertex colour. */
void nv2a_gles_tri(const float c0[4], const float c1[4], const float c2[4],
                   uint32_t argb);

/* Producer (parser thread). Register (or refresh) a decoded texture under a
 * stable id. `rgba8` is w*h texels, 4 bytes each in R,G,B,A order (ready for
 * glTexImage2D GL_RGBA/GL_UNSIGNED_BYTE); the module copies it. addr_u/addr_v
 * are the NV097_SET_TEXTURE_ADDRESS codes (1=wrap, 3=clamp, else clamp). Called
 * on the parser thread while the texture's guest state is still live; the GL
 * upload happens lazily on the presenter thread. A texid already known is a
 * no-op (Phase 1 assumes textures are static at a given identity). */
void nv2a_gles_texture(uint32_t texid, int w, int h,
                       int addr_u, int addr_v, const void *rgba8);

/* Producer (parser thread). One textured triangle in CLIP space. uvN are
 * NORMALISED [0,1] texture coordinates; texid selects a texture registered via
 * nv2a_gles_texture (0 = untextured, falls back to the flat argb colour). */
void nv2a_gles_tri_tex(const float c0[4], const float c1[4], const float c2[4],
                       const float uv0[2], const float uv1[2], const float uv2[2],
                       uint32_t argb, uint32_t texid);

/* Producer (parser thread). Guest buffer flip: publish the built batch. */
void nv2a_gles_frame(void);

/* Consumer (presenter GL thread, context current). Draw the last published
 * batch into an FBO with a depth buffer and composite it over the default
 * framebuffer. surf_w/surf_h are the window surface dimensions. */
/* Draw the latest published 3D frame and composite it into the destination
 * rectangle (vx,vy,vw,vh) of the current default framebuffer -- the same
 * rectangle the guest framebuffer was blitted to (aspect-correct letterbox). */
void nv2a_gles_render(int vx, int vy, int vw, int vh);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_PGRAPH_GLES_H */
