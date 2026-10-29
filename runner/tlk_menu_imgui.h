/* tlk_menu_imgui.h -- C entry points for tlk_menu_imgui.cpp (Dear ImGui glue
 * for recomp-ui's in-game menu). */
#pragma once

#include <SDL3/SDL.h>

#include "recomp_runtime_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* With ren NULL, draws through the OpenGL presenter, whose context must be current. 0 if ImGui could not start. */
int TlkMenuImguiInit(SDL_Window *win, SDL_Renderer *ren, const char *font_path, float font_px);
void TlkMenuImguiEvent(const SDL_Event *ev);
/* The menu was opened: ignore the mouse until it moves. */
void TlkMenuImguiOpened(void);
/* One ImGui frame holding the menu, drawn into the bound framebuffer or the renderer. With
 * *about set, the About window is drawn over it; its Close button clears *about. */
void TlkMenuImguiDraw(RecompRuntimeUi *ui, int *about);
void TlkMenuImguiShutdown(void);

#ifdef __cplusplus
}
#endif
