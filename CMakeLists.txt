cmake_minimum_required(VERSION 3.20)
project(TLKRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Linux builds require _GNU_SOURCE for popen / readlink in strict C mode.
add_compile_definitions(_GNU_SOURCE)

set(SNESRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to snesrecomp framework submodule")
set(TLK_SNESRECOMP_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/snesrecomp" CACHE PATH
    "Directory containing generated snesrecomp C sources")

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS
    "${TLK_SNESRECOMP_GEN_DIR}/*.c")

if(NOT GENERATED_SOURCES)
    message(WARNING
        "No generated sources found in ${TLK_SNESRECOMP_GEN_DIR}. "
        "Run `python3 scripts/generate_snesrecomp.py` to generate them.")
endif()

# Static library containing recompiled game functions
add_library(snesrecomp_game STATIC ${GENERATED_SOURCES})
target_include_directories(snesrecomp_game PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runner"
    "${SNESRECOMP_ROOT}/runner/src"
    "${SNESRECOMP_ROOT}/runner/src/snes")

if(NOT MSVC)
    target_compile_options(snesrecomp_game PRIVATE
        -w -Wno-implicit-function-declaration)
endif()

# ── Hosts ─────────────────────────────────────────────────────────────────
# tlk_headless: windowless validation host (runner/headless_main.c).
# tlk_sdl:      playable SDL3 host (runner/sdl_main.c) on the headless
#               emulation path: window, audio, pacing, input.
# Both compile the runner, the generated C and the TLK adapter themselves;
# SNESRECOMP_ENABLE_MODS must precede the include (runner.cmake reads it).
option(TLK_BUILD_HOSTS "Build tlk_headless and tlk_sdl" ON)
# TLK_STATIC_SDL: ignore any installed SDL3 and link the pinned SDL3 release
# (snesrecomp's SNESRECOMP_SDL3_FETCH, 3.4.10, sha256-checked) statically, so
# the player tarball runs without SDL3 installed (scripts/package.sh). The
# video/audio/input backends are compiled in and dlopen their system libraries
# at run time. Default OFF: dev builds keep using the system SDL3.
option(TLK_STATIC_SDL "Build and statically link the pinned SDL3 release" OFF)
if(TLK_STATIC_SDL)
    set(CMAKE_DISABLE_FIND_PACKAGE_SDL3 ON)
    set(SNESRECOMP_SDL3_FETCH ON CACHE BOOL "" FORCE)
    set(SNESRECOMP_SDL_BACKEND "SDL3" CACHE STRING "" FORCE)
    set(_tlk_sdl_opts SDL_VIDEO SDL_AUDIO SDL_JOYSTICK SDL_HIDAPI SDL_HIDAPI_JOYSTICK SDL_OPENGL)
    # Windows (MinGW cross build, cmake/mingw-w64-x86_64.cmake) uses SDL's
    # own Win32 backends; the Linux ones below do not exist there.
    if(NOT WIN32)
        list(APPEND _tlk_sdl_opts
             SDL_WAYLAND SDL_WAYLAND_SHARED SDL_X11 SDL_X11_SHARED
             SDL_PIPEWIRE SDL_PIPEWIRE_SHARED SDL_PULSEAUDIO SDL_PULSEAUDIO_SHARED
             SDL_ALSA SDL_ALSA_SHARED SDL_LIBUDEV SDL_DBUS SDL_KMSDRM)
    endif()
    foreach(_opt IN LISTS _tlk_sdl_opts)
        set(${_opt} ON CACHE BOOL "" FORCE)
    endforeach()
endif()
if(TLK_BUILD_HOSTS)
    if(NOT EXISTS "${SNESRECOMP_ROOT}/runner/runner.cmake")
        message(FATAL_ERROR "snesrecomp submodule missing at ${SNESRECOMP_ROOT}")
    endif()
    set(SNESRECOMP_ENABLE_MODS ON CACHE BOOL
        "Mod package loader + trusted plugins (required by the desktop host)" FORCE)
    include("${SNESRECOMP_ROOT}/runner/runner.cmake")
    # Runtime adaptations: patched build-dir copies of pinned snesrecomp
    # sources (cmake/runtime-patches/*.hunks); the submodule stays untouched.
    include(cmake/TlkRuntimePatches.cmake)
    tlk_use_patched_runtime_sources(SNESRECOMP_RUNNER_SOURCES)

    set(TLK_RUNNER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runner")
    set(TLK_COMMON_SOURCES
        ${SNESRECOMP_RUNNER_SOURCES}
        ${TLK_RUNNER_DIR}/tlk_game.c
        ${TLK_RUNNER_DIR}/tlk_decomp.c
        ${TLK_RUNNER_DIR}/aot_pace.c
        ${TLK_RUNNER_DIR}/tlk_fiber.c
        ${TLK_RUNNER_DIR}/tlk_savestate.c
        ${TLK_RUNNER_DIR}/host_contract.c)

    function(tlk_configure_target target)
        snesrecomp_target_generated_code(${target} "${TLK_SNESRECOMP_GEN_DIR}")
        target_include_directories(${target} PRIVATE
            ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
            ${SNESRECOMP_ROOT}/runner/src/desktop
            ${TLK_RUNNER_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/recomp)
        target_link_libraries(${target} PRIVATE ${SNESRECOMP_RUNNER_LIBRARIES})
        target_compile_definitions(${target} PRIVATE
            SNES_GAME_VERSION="dev"
            SNES_GAME_TITLE="The Lion King")
        snesrecomp_rom_identity(${target} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rom_identity.txt")
    endfunction()

    add_executable(tlk_headless ${TLK_COMMON_SOURCES}
        ${TLK_RUNNER_DIR}/headless_main.c
        ${TLK_RUNNER_DIR}/tlk_rommap.c
        ${TLK_RUNNER_DIR}/tlk_callpair.c
        ${TLK_RUNNER_DIR}/headless_host.c
        ${TLK_RUNNER_DIR}/verified_rom.c)
    tlk_configure_target(tlk_headless)
    snesrecomp_target_mmx_config(tlk_headless)
    # common_rtl.c includes the SDL compat header; no window is ever opened.
    snesrecomp_target_sdl(tlk_headless)

    # tlk_sdl: same sources and host contract as tlk_headless (so the same
    # frame driver and APU timeline), plus a small SDL3 front end for window,
    # audio, pacing and input. Not the framework desktop host: that one turns
    # on extended frame timing, which bypasses TLK's NTSC SPC ratio.
    add_executable(tlk_sdl ${TLK_COMMON_SOURCES}
        ${TLK_RUNNER_DIR}/sdl_main.c
        ${TLK_RUNNER_DIR}/tlk_video.c
        ${TLK_RUNNER_DIR}/headless_host.c
        ${TLK_RUNNER_DIR}/verified_rom.c)
    tlk_configure_target(tlk_sdl)
    snesrecomp_target_sdl(tlk_sdl)
    # OpenGL presenter + GLSL presets (CC0), staged to build/assets/shaders.
    snesrecomp_target_opengl(tlk_sdl)
    snesrecomp_target_shader_presets(tlk_sdl)
    tlk_use_patched_target_sources(tlk_sdl)
    # In-game settings menu: recomp-ui's runtime menu (MIT, submodule), drawn
    # with Dear ImGui over the OpenGL presenter (runner/tlk_menu_imgui.cpp).
    # Its fonts and images are staged to build/assets/fonts and assets/img.
    # FreeType/HarfBuzz would only colour netplay-chat emoji, and the release
    # binary must not need either installed: keep recomp-ui from finding them.
    set(CMAKE_DISABLE_FIND_PACKAGE_Freetype ON)
    set(RECOMP_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui" CACHE PATH "" FORCE)
    include("${RECOMP_UI_ROOT}/recomp_ui.cmake")
    target_sources(tlk_sdl PRIVATE ${TLK_RUNNER_DIR}/tlk_menu_imgui.cpp)
    recomp_target_launcher_ui(tlk_sdl CONSOLE snes)
endif()

# ── Install / packaging ───────────────────────────────────────────────────
# `cmake --install build --prefix DIR` installs the player binary and the
# notes that go with it. scripts/package.sh turns that into a tarball. The
# ROM, extracted assets and the generated C are never installed.
if(TLK_BUILD_HOSTS)
    include(GNUInstallDirs)
    install(TARGETS tlk_sdl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    # Shader presets are looked up next to the binary (assets/shaders/...).
    install(DIRECTORY ${SNESRECOMP_ROOT}/runner/assets/shaders
            DESTINATION ${CMAKE_INSTALL_BINDIR}/assets)
    install(FILES packaging/README.txt packaging/LICENSE-NOTES.txt
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(FILES snesrecomp/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR}
            RENAME LICENSE-snesrecomp.txt)
    # The settings menu: recomp-ui (MIT) with Dear ImGui (MIT), and the one
    # font it draws with (Lato, OFL 1.1), looked up in assets/fonts.
    install(FILES ${RECOMP_UI_ROOT}/assets/common/fonts/LatoLatin-Regular.ttf
            DESTINATION ${CMAKE_INSTALL_BINDIR}/assets/fonts)
    install(FILES ${RECOMP_UI_ROOT}/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR}
            RENAME LICENSE-recomp-ui.txt)
    install(FILES packaging/LICENSE-imgui.txt packaging/LICENSE-Lato.txt
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
    if(TLK_STATIC_SDL)
        include(FetchContent)
        FetchContent_GetProperties(SDL3)   # sdl3_SOURCE_DIR (set in a function)
        # SDL3 is zlib-licensed; its licence goes with the binary it is in.
        install(FILES "${sdl3_SOURCE_DIR}/LICENSE.txt" DESTINATION ${CMAKE_INSTALL_DOCDIR}
                RENAME LICENSE-SDL3.txt)
    endif()
endif()
