/* tlk_gpu_shader.h -- shader presets on SDL's GPU renderer (Vulkan on Android). */
#pragma once

#include <SDL3/SDL.h>

/* 1 when ren is SDL's GPU renderer, it takes SPIR-V, and the build compiled presets in. */
int TlkGpuShaderInit(SDL_Renderer *ren);
/* Picks the compiled preset named by the file name of path (".../pvm.glslp" is "pvm"); "" or NULL is none.
 * 0 when path names a preset this build didn't compile, or the shader didn't load: then none is active. */
int TlkGpuShaderSelect(SDL_Renderer *ren, const char *path);
int TlkGpuShaderActive(void);
/* Whether the active preset samples the game linearly (its filter_linear0). */
int TlkGpuShaderFilterLinear(void);
/* Sets a #pragma parameter of the active preset, clamped to its range; 0 when it has none of that name. */
int TlkGpuShaderSetParam(const char *name, float value);
/* Sets the uniforms and the render state for the next draw of an in_w x in_h texture into out_w x out_h pixels.
 * SDL_SetGPURenderState(ren, NULL) after that draw puts SDL's own shader back. */
void TlkGpuShaderBegin(SDL_Renderer *ren, int in_w, int in_h, int out_w, int out_h);
void TlkGpuShaderQuit(void);
