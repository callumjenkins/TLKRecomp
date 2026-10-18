/* tlk_video.c -- g_config for snesrecomp's desktop OpenGL presenter.
 *
 * desktop/opengl.c and glsl_shader.c read the shader path, display aspect and
 * filtering from the shared desktop Config. config.h pulls in sdl_compat.h,
 * which needs SDL3's old-name aliases, so it stays out of sdl_main.c; this file
 * owns the struct and sdl_main.c sets it through TlkVideoConfig. */
#include "config.h"

Config g_config;

void TlkVideoConfig(const char *shader, int aspect, int smooth) {
    g_config.shader = shader && shader[0] ? shader : NULL;
    g_config.display_aspect = (uint8)SnesDisplayAspect_Clamp(aspect);
    g_config.linear_filtering = smooth != 0;
    g_config.ignore_aspect_ratio = false;
}
