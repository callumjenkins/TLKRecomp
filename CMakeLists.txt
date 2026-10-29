cmake_minimum_required(VERSION 3.20)
project(TLKRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Cache repeated compiles, including regenerated sources whose contents did
# not change. Respect an explicitly configured launcher (including an empty
# one), and keep ccache optional for machines that do not have it.
option(TLK_USE_CCACHE "Use ccache when available" ON)
if(TLK_USE_CCACHE)
    find_program(TLK_CCACHE_PROGRAM ccache)
    if(TLK_CCACHE_PROGRAM)
        foreach(lang C CXX)
            if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER AND
               NOT DEFINED ENV{CMAKE_${lang}_COMPILER_LAUNCHER})
                set(CMAKE_${lang}_COMPILER_LAUNCHER "${TLK_CCACHE_PROGRAM}")
            endif()
        endforeach()
    endif()
endif()

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

# Compile the game once for both hosts. An OBJECT library also retains the
# generated module-registration constructor, which a static archive linker
# could otherwise discard because no host references that object directly.
add_library(snesrecomp_game OBJECT)
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
# Both compile the runner and TLK adapter, and share the generated objects;
# SNESRECOMP_ENABLE_MODS must precede the include (runner.cmake reads it).
option(TLK_BUILD_HOSTS "Build tlk_headless and tlk_sdl" ON)
# TLK_STATIC_SDL: ignore any installed SDL3 and link the pinned SDL3 release
# (snesrecomp's SNESRECOMP_SDL3_FETCH, 3.4.10, sha256-checked) statically, so
# the player tarball runs without SDL3 installed (scripts/package.sh). The
# video/audio/input backends are compiled in and dlopen their system libraries
# at run time. Default OFF: dev builds keep using the system SDL3.
option(TLK_STATIC_SDL "Build and statically link the pinned SDL3 release" OFF)
# TLK_FIBER_COMPAT: run the fibers through snesrecomp's fiber_compat, as Android
# does, so the Linux gates can run tlk_fiber.c's fiber API path.
option(TLK_FIBER_COMPAT "Use fiber_compat for TLK's fibers (always on for Android)" OFF)
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
    snesrecomp_target_generated_code(snesrecomp_game "${TLK_SNESRECOMP_GEN_DIR}")
    if(GENERATED_SOURCES)
        target_compile_definitions(snesrecomp_game INTERFACE SNESRECOMP_HAS_GENERATED_CODE=1)
    else()
        target_compile_definitions(snesrecomp_game INTERFACE SNESRECOMP_SETUP_HOST=1)
    endif()
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
        ${TLK_RUNNER_DIR}/tlk_wide.c
        ${TLK_RUNNER_DIR}/host_contract.c)

    function(tlk_configure_target target)
        target_include_directories(${target} PRIVATE
            ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
            ${SNESRECOMP_ROOT}/runner/src/desktop
            ${TLK_RUNNER_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/recomp)
        target_link_libraries(${target} PRIVATE snesrecomp_game ${SNESRECOMP_RUNNER_LIBRARIES})
        target_compile_definitions(${target} PRIVATE
            SNES_GAME_VERSION="dev"
            SNES_GAME_TITLE="The Lion King")
        snesrecomp_rom_identity(${target} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rom_identity.txt")
        if(ANDROID OR TLK_FIBER_COMPAT)
            snesrecomp_target_fiber_compat(${target})
        endif()
        if(TLK_FIBER_COMPAT)
            target_compile_definitions(${target} PRIVATE TLK_FIBER_COMPAT=1)
        endif()
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
    set(TLK_SDL_SOURCES ${TLK_COMMON_SOURCES}
        ${TLK_RUNNER_DIR}/sdl_main.c
        ${TLK_RUNNER_DIR}/tlk_touch.c
        ${TLK_RUNNER_DIR}/tlk_video.c
        ${TLK_RUNNER_DIR}/tlk_assets.c
        ${TLK_RUNNER_DIR}/headless_host.c
        ${TLK_RUNNER_DIR}/verified_rom.c)
    if(ANDROID)
        # SDLActivity loads the game as libmain.so and calls its main.
        add_library(tlk_sdl SHARED ${TLK_SDL_SOURCES})
        set_target_properties(tlk_sdl PROPERTIES OUTPUT_NAME main)
        target_link_libraries(tlk_sdl PRIVATE android log)
        # A missing symbol would otherwise surface only when the phone loads the library.
        target_link_options(tlk_sdl PRIVATE -Wl,--no-undefined)
    else()
        add_executable(tlk_sdl ${TLK_SDL_SOURCES})
    endif()
    tlk_configure_target(tlk_sdl)
    snesrecomp_target_sdl(tlk_sdl)
    # OpenGL presenter + GLSL presets (CC0), staged to build/assets/shaders.
    # Android draws through SDL_Renderer on GLES instead.
    if(NOT ANDROID)
        snesrecomp_target_opengl(tlk_sdl)
        snesrecomp_target_shader_presets(tlk_sdl)
        tlk_use_patched_target_sources(tlk_sdl)
    endif()
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
    if(ANDROID)
        # recomp-ui's Android branch calls SDL_AndroidGetInternalStoragePath, SDL2's name for it.
        set_source_files_properties(
            "${RECOMP_UI_ROOT}/src/common/backends/imgui/launcher_imgui.cpp"
            TARGET_DIRECTORY tlk_sdl PROPERTIES COMPILE_DEFINITIONS SDL_ENABLE_OLD_NAMES=1)
    endif()
    # The font and shader presets, compiled in for the single Windows .exe
    # (runner/tlk_assets.c). The font comes first: it is the file probed.
    include(cmake/TlkEmbedAssets.cmake)
    set(tlk_shader_dir "${SNESRECOMP_ROOT}/runner/assets/shaders")
    file(GLOB_RECURSE tlk_shader_files RELATIVE "${tlk_shader_dir}" CONFIGURE_DEPENDS
         "${tlk_shader_dir}/*.glslp" "${tlk_shader_dir}/*.glsl")
    set(tlk_embedded
        "assets/fonts/LatoLatin-Regular.ttf=${RECOMP_UI_ROOT}/assets/common/fonts/LatoLatin-Regular.ttf")
    foreach(f IN LISTS tlk_shader_files)
        list(APPEND tlk_embedded "assets/shaders/${f}=${tlk_shader_dir}/${f}")
    endforeach()
    tlk_embed_assets(tlk_sdl "${CMAKE_CURRENT_BINARY_DIR}/tlk_assets_data.c" ${tlk_embedded})
    # Windows: the app icon (packaging/icon, tools/make_icon.py).
    if(WIN32)
        enable_language(RC)
        target_sources(tlk_sdl PRIVATE packaging/tlk_sdl.rc)
    endif()
else()
    target_sources(snesrecomp_game PRIVATE ${GENERATED_SOURCES})
endif()

# ── Tests that need no ROM ────────────────────────────────────────────────
if(NOT ANDROID)
    enable_testing()
    add_executable(tlk_touch_test tests/tlk_touch_test.c runner/tlk_touch.c)
    target_include_directories(tlk_touch_test PRIVATE runner)
    if(NOT MSVC)
        target_link_libraries(tlk_touch_test PRIVATE m)
    endif()
    add_test(NAME tlk_touch COMMAND tlk_touch_test)

    # The fiber switches on each backend: plain ucontext, fiber_compat on ucontext, and fiber_compat's
    # parked threads, which only Android builds otherwise.
    set(tlk_fiber_fc "${SNESRECOMP_ROOT}/runner/src/desktop")
    foreach(v IN ITEMS ucontext compat android)
        set(t tlk_fiber_test_${v})
        add_executable(${t} tests/tlk_fiber_test.c runner/tlk_fiber.c)
        target_include_directories(${t} PRIVATE ${SNESRECOMP_ROOT}/runner/src)
        if(NOT v STREQUAL ucontext)
            target_sources(${t} PRIVATE ${tlk_fiber_fc}/fiber_compat.c)
            target_include_directories(${t} PRIVATE ${tlk_fiber_fc})
            target_compile_definitions(${t} PRIVATE TLK_FIBER_COMPAT=1)
        endif()
        if(v STREQUAL android)
            target_compile_definitions(${t} PRIVATE __ANDROID__=1)
            find_package(Threads REQUIRED)
            target_link_libraries(${t} PRIVATE Threads::Threads)
        endif()
        add_test(NAME tlk_fiber_${v} COMMAND ${t})
    endforeach()
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
