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
#include "launcher_theme.h"
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

static ImVec4 Col(const LngColor &c) { return ImVec4(c.r, c.g, c.b, c.a); }

/* The About window: why this port exists, by whom, and where the source is.
 * Centred over the menu in the menu's own theme; the links open the browser
 * (mouse); the About section's rows do the same from the keyboard or a pad. */
static void DrawAbout(int *about) {
    static const LauncherTheme t = launcher_theme_by_name("snes");
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_lg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(t.spacing_lg * 1.5f, t.spacing_lg * 1.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(t.spacing_sm, t.spacing_md));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(t.panel));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(t.accent2));
    ImGui::PushStyleColor(ImGuiCol_Separator, Col(t.border));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(t.text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Col(t.text_muted));
    ImGui::PushStyleColor(ImGuiCol_TextLink, Col(t.accent2));
    ImGui::PushStyleColor(ImGuiCol_Button, Col(t.control));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(t.control_hovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(t.accent_dim));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##tlk-about", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col(t.accent2));
        ImGui::TextUnformatted("About");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::TextWrapped("When I was around 8 years old, my family and I would gather around the kitchen TV to "
                           "try to beat this game. We still love it, and it's an important part of our family "
                           "lore.");
        ImGui::TextWrapped("This effort is a tribute: to my family, to my kid self, and to the community, which "
                           "I'm sure will enjoy it.");
        ImGui::TextUnformatted("Alejandro AR");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("kinduff.com", "https://kinduff.com");
        ImGui::TextDisabled("Source:");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("github.com/kinduff/TLKRecomp", "https://github.com/kinduff/TLKRecomp");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Esc/B  Close");
        const char *close = "Close";
        const float close_w = ImGui::CalcTextSize(close).x + t.spacing_lg * 2.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - close_w);
        if (ImGui::Button(close, ImVec2(close_w, 0.0f))) *about = 0;
    }
    ImGui::End();
    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(4);
}

extern "C" void TlkMenuImguiDraw(RecompRuntimeUi *ui, int *about) {
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
    if (*about) DrawAbout(about);
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
