/* tlk_menu_imgui.cpp -- Dear ImGui glue for tlk_sdl's in-game menu.
 *
 * The menu itself (rows, navigation, status line, look) is recomp-ui's
 * runtime menu (recomp-ui/src/recomp_runtime_ui.h); sdl_main.c owns its rows
 * and callbacks. This file only gives it an ImGui frame to draw in: one
 * context on the OpenGL presenter's window, fed SDL events, rendered from the
 * presenter's overlay hook between the game picture and the swap. ImGui's own
 * keyboard/gamepad navigation stays off; sdl_main.c drives the menu. */
#include "tlk_menu_imgui.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <cstdio>

static bool s_ready;
static SDL_Window *s_win;

/* recomp-ui's desktop layout needs about 768x672 (the window at scale 3);
 * smaller, its footer runs into the Resume button. A smaller window gets the
 * menu laid out at that size and shrunk to fit: s_shrink < 1. */
enum { kLayoutW = 768, kLayoutH = 672 };
static float s_shrink = 1.0f;

static void UpdateShrink(void) {
    int w = 0, h = 0;
    SDL_GetWindowSize(s_win, &w, &h);
    float k = 1.0f;
    if (w > 0 && w < kLayoutW) k = (float)w / kLayoutW;
    if (h > 0 && (float)h / kLayoutH < k) k = (float)h / kLayoutH;
    s_shrink = k;
}

extern "C" int TlkMenuImguiInit(SDL_Window *win, const char *font_path, float font_px) {
    if (s_ready) return 1;
    SDL_GLContext ctx = SDL_GL_GetCurrentContext();
    if (!ctx) return 0;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   /* nothing to remember: the menu lays itself out */
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse;   /* until the mouse moves (UseMouse) */
    /* Sizes stay in window units; on a HiDPI display the glyphs are
     * rasterized at its pixel density so they are sharp, not magnified. */
    ImFontConfig cfg;
    float density = SDL_GetWindowPixelDensity(win);
    cfg.RasterizerDensity = density > 0.0f ? density : 1.0f;
    ImFont *font = nullptr;
    if (font_path) {
        if (FILE *f = std::fopen(font_path, "rb")) {
            std::fclose(f);
            font = io.Fonts->AddFontFromFileTTF(font_path, font_px, &cfg);
        }
    }
    if (!font) {
        cfg.SizePixels = font_px;
        io.Fonts->AddFontDefault(&cfg);
        std::fprintf(stderr, "menu: font %s not found, using ImGui's own\n", font_path ? font_path : "(none)");
    }
    if (!ImGui_ImplSDL3_InitForOpenGL(win, ctx) || !ImGui_ImplOpenGL3_Init("#version 330")) {
        ImGui::DestroyContext();
        return 0;
    }
    s_win = win;
    s_ready = true;
    return 1;
}

/* recomp-ui selects the row under the mouse pointer every frame, so a pointer
 * that merely rests over the window would undo every key press. The mouse
 * counts only from the moment it is moved or clicked, until the next key or
 * pad button. */
static void UseMouse(bool on) {
    ImGuiIO &io = ImGui::GetIO();
    if (on) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
}

extern "C" void TlkMenuImguiEvent(const SDL_Event *ev) {
    if (!s_ready) return;
    switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL: UseMouse(true); break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: UseMouse(false); break;
    default: break;
    }
    if (s_shrink < 1.0f && (ev->type == SDL_EVENT_MOUSE_MOTION || ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            ev->type == SDL_EVENT_MOUSE_BUTTON_UP)) {
        SDL_Event e = *ev;   /* window coordinates -> the shrunk layout's */
        if (e.type == SDL_EVENT_MOUSE_MOTION) { e.motion.x /= s_shrink; e.motion.y /= s_shrink; }
        else { e.button.x /= s_shrink; e.button.y /= s_shrink; }
        ImGui_ImplSDL3_ProcessEvent(&e);
        return;
    }
    ImGui_ImplSDL3_ProcessEvent(ev);
}

extern "C" void TlkMenuImguiOpened(void) {
    if (s_ready) UseMouse(false);
}

extern "C" void TlkMenuImguiDraw(RecompRuntimeUi *ui) {
    if (!s_ready) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    UpdateShrink();
    if (s_shrink < 1.0f) {
        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize.x /= s_shrink;
        io.DisplaySize.y /= s_shrink;
        io.DisplayFramebufferScale.x *= s_shrink;
        io.DisplayFramebufferScale.y *= s_shrink;
    }
    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(ui);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

extern "C" void TlkMenuImguiShutdown(void) {
    if (!s_ready) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    s_ready = false;
}
