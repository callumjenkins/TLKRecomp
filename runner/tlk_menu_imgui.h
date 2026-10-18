/* tlk_menu_imgui.h -- C entry points for tlk_menu_imgui.cpp (Dear ImGui glue
 * for recomp-ui's in-game menu on the OpenGL presenter). */
#pragma once

#include <SDL3/SDL.h>

#include "recomp_runtime_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call with the presenter's GL context current. 0 if ImGui could not start. */
int TlkMenuImguiInit(SDL_Window *win, const char *font_path, float font_px);
void TlkMenuImguiEvent(const SDL_Event *ev);
/* The menu was opened: ignore the mouse until it moves. */
void TlkMenuImguiOpened(void);
/* One ImGui frame holding the menu, drawn into the bound framebuffer. */
void TlkMenuImguiDraw(RecompRuntimeUi *ui);
void TlkMenuImguiShutdown(void);

#ifdef __cplusplus
}
#endif
