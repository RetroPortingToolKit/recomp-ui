# recomp-ui — console-agnostic Dear ImGui pre-boot launcher, shared across
# recomp ecosystems (SNES, NES, N64, PSX, ...).
#
# This top-level CMakeLists.txt builds ONLY a standalone self-test exe
# (recomp-ui-launcher, from src/proto_main.c) so the repo can verify itself
# in isolation. Real consumers do NOT build this project directly — they
# `include(recomp_ui.cmake)` from their own CMake project and call
# `recomp_target_launcher_ui(<their_target>)` (see recomp_ui.cmake and
# README.md).
#
# Build (mingw + ninja):
#   cmake -G Ninja -S . -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
#   cmake --build build

cmake_minimum_required(VERSION 3.24)
project(recomp_ui C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SNESRECOMP_SDL_BACKEND "SDL3" CACHE STRING
    "Standalone launcher SDL backend (SDL3 or SDL2)")
set_property(CACHE SNESRECOMP_SDL_BACKEND PROPERTY STRINGS SDL3 SDL2)
string(TOUPPER "${SNESRECOMP_SDL_BACKEND}" SNESRECOMP_SDL_BACKEND)
if(SNESRECOMP_SDL_BACKEND STREQUAL "SDL3")
    find_package(SDL3 CONFIG REQUIRED)
    set(RUI_SDL_TARGET SDL3::SDL3)
    # Any target that compiles a header off the launcher_platform.h chain needs
    # these, or that header falls through to <SDL.h> -- the SDL2 include layout --
    # and SDL_main's WinMain shim can rewrite standalone test entry points.
    # recomp_target_launcher_ui() sets the launcher up; standalone targets that
    # only link RUI_SDL_TARGET must be given the same contract explicitly.
    set(RUI_SDL_DEFS LNG_SDL3=1 SDL_MAIN_HANDLED=1)
    if(TARGET SDL3::SDL3-shared)
        set(RUI_SDL_RUNTIME_TARGET SDL3::SDL3-shared)
    endif()
elseif(SNESRECOMP_SDL_BACKEND STREQUAL "SDL2")
    find_package(SDL2 REQUIRED)
    set(RUI_SDL_TARGET SDL2::SDL2)
    get_target_property(RUI_SDL2_TARGET_TYPE SDL2::SDL2 TYPE)
    if(RUI_SDL2_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
        set(RUI_SDL_RUNTIME_TARGET SDL2::SDL2)
    endif()
else()
    message(FATAL_ERROR "SNESRECOMP_SDL_BACKEND must be SDL3 or SDL2")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/recomp_gl.cmake)
if(WIN32)
    set(RUI_GL_LIBS opengl32)
else()
    recomp_resolve_gl(RUI_GL_TARGET)
    set(RUI_GL_LIBS ${RUI_GL_TARGET} ${CMAKE_DL_LIBS})
endif()

add_executable(recomp-ui-launcher src/proto_main.c)
target_link_libraries(recomp-ui-launcher PRIVATE ${RUI_SDL_TARGET} ${RUI_GL_LIBS})
if(WIN32 AND RUI_SDL_RUNTIME_TARGET)
    add_custom_command(TARGET recomp-ui-launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${RUI_SDL_RUNTIME_TARGET}>
            $<TARGET_FILE_DIR:recomp-ui-launcher>)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/recomp_ui.cmake)
recomp_target_launcher_ui(recomp-ui-launcher CONSOLE all)

include(CTest)
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(recomp-ui-launcher-mod-commit-job-test
        tests/launcher_mod_commit_job_test.cpp)
    target_include_directories(recomp-ui-launcher-mod-commit-job-test PRIVATE src/common)
    target_link_libraries(recomp-ui-launcher-mod-commit-job-test PRIVATE Threads::Threads)
    add_test(NAME recomp-ui-launcher-mod-commit-job
        COMMAND $<TARGET_FILE:recomp-ui-launcher-mod-commit-job-test>)

    add_executable(recomp-ui-launcher-mod-preparation-test
        tests/launcher_mod_preparation_test.cpp)
    target_include_directories(recomp-ui-launcher-mod-preparation-test PRIVATE src/common)
    target_link_libraries(recomp-ui-launcher-mod-preparation-test PRIVATE Threads::Threads)
    add_test(NAME recomp-ui-launcher-mod-preparation
        COMMAND $<TARGET_FILE:recomp-ui-launcher-mod-preparation-test>)

    add_executable(recomp-ui-launcher-debug-pending-test
        tests/launcher_debug_pending_test.c src/common/launcher_debug.c)
    target_include_directories(recomp-ui-launcher-debug-pending-test PRIVATE src src/common)
    target_compile_definitions(recomp-ui-launcher-debug-pending-test PRIVATE ${RUI_SDL_DEFS})
    target_link_libraries(recomp-ui-launcher-debug-pending-test PRIVATE ${RUI_SDL_TARGET} ${RUI_GL_LIBS})
    foreach(_rui_pending_case wait model launch)
        add_test(NAME recomp-ui-launcher-debug-pending-${_rui_pending_case}
            COMMAND $<TARGET_FILE:recomp-ui-launcher-debug-pending-test> ${_rui_pending_case})
    endforeach()

    # Real desktop/backend evidence is opt-in, not a headless CI requirement.
    add_executable(recomp-ui-worker-commit-probe EXCLUDE_FROM_ALL
        tests/launcher_worker_commit_probe.cpp)
    target_link_libraries(recomp-ui-worker-commit-probe PRIVATE ${RUI_SDL_TARGET} ${RUI_GL_LIBS})
    set(_rui_saved_mods ${RECOMP_UI_ENABLE_MODS})
    set(RECOMP_UI_ENABLE_MODS ON)
    recomp_target_launcher_ui(recomp-ui-worker-commit-probe CONSOLE all)
    set(RECOMP_UI_ENABLE_MODS ${_rui_saved_mods})
    if(WIN32 AND RUI_SDL_RUNTIME_TARGET)
        add_custom_command(TARGET recomp-ui-worker-commit-probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${RUI_SDL_RUNTIME_TARGET}>
                $<TARGET_FILE_DIR:recomp-ui-worker-commit-probe>)
    endif()

    add_executable(recomp-ui-directory-url-test tests/launcher_directory_url_test.cpp)
    target_include_directories(recomp-ui-directory-url-test PRIVATE src/common)
    add_test(NAME recomp-ui-directory-url COMMAND $<TARGET_FILE:recomp-ui-directory-url-test>)
    add_executable(recomp-ui-ips-patch-test
        tests/ips_patch_test.c
        src/common/ips_patch.c
        src/common/crc32.c)
    target_include_directories(recomp-ui-ips-patch-test PRIVATE src/common)
    add_test(NAME recomp-ui-ips-patch COMMAND $<TARGET_FILE:recomp-ui-ips-patch-test>)

    # The photosensitivity flash guard. Pure arithmetic on two ARGB frames --
    # no SDL, no window, no ROM -- so it runs anywhere the rest of these do.
    add_executable(recomp-ui-flash-guard-test
        tests/flash_guard_test.c
        src/common/recomp_flash_guard.c)
    target_include_directories(recomp-ui-flash-guard-test PRIVATE src)
    add_test(NAME recomp-ui-flash-guard
             COMMAND $<TARGET_FILE:recomp-ui-flash-guard-test>)

    # What the Linux native file picker hands to the zenity/kdialog it spawns,
    # and how it reads that backend's exit code. Uses stub backends on a
    # private PATH -- no display, no GTK, no real zenity -- so it runs
    # wherever the others do. Guards the AppImage bug where the bundle's
    # LD_LIBRARY_PATH leaked into the host zenity and its failure exit was
    # read back as "the user cancelled".
    add_executable(recomp-ui-launcher-files-spawn-test
        tests/launcher_files_spawn_test.c
        src/common/launcher_files.c
        src/third_party/tinyfiledialogs.c)
    target_include_directories(recomp-ui-launcher-files-spawn-test PRIVATE
        src src/common)
    add_test(
        NAME recomp-ui-launcher-files-spawn
        COMMAND $<TARGET_FILE:recomp-ui-launcher-files-spawn-test>
                "${CMAKE_CURRENT_BINARY_DIR}")

    # The blocking launcher_pick_* wrappers must not come back into a UI
    # backend: they hide "no usable native picker" as "user cancelled", which
    # is how 14 Browse buttons shipped dead on Linux. ui_pick() is the only
    # sanctioned entry point; this keeps a regression loud instead of silent.
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(
            NAME recomp-ui-no-raw-pickers
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_raw_pickers.py"
                "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        message(WARNING
            "Python3 not found: recomp-ui-no-raw-pickers picker-API guard is "
            "NOT registered. A UI backend could reintroduce a silent dead "
            "Browse button without failing the suite.")
    endif()

    add_executable(recomp-ui-rom-patch-tool
        tools/rom_patch_tool.c
        src/common/ips_patch.c
        src/common/crc32.c
        src/common/sha1.c)
    target_include_directories(recomp-ui-rom-patch-tool PRIVATE src/common)

    add_test(
        NAME recomp-ui-asset-manifests
        COMMAND ${CMAKE_COMMAND}
            "-DRECOMP_UI_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/asset_manifest_test.cmake)

    add_executable(recomp-ui-psx-asset-stage-probe
        tests/asset_stage_probe.c)
    set_target_properties(
        recomp-ui-psx-asset-stage-probe
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/asset-stage-psx")
    add_custom_command(TARGET recomp-ui-psx-asset-stage-probe PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:recomp-ui-psx-asset-stage-probe>/assets/img"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RUI_ASSETS}/consoles/n64/img/cart_red.tga"
                "$<TARGET_FILE_DIR:recomp-ui-psx-asset-stage-probe>/assets/img/"
        VERBATIM)
    _recomp_ui_stage_assets(recomp-ui-psx-asset-stage-probe psx)
    add_test(
        NAME recomp-ui-psx-asset-staging
        COMMAND ${CMAKE_COMMAND}
            "-DASSET_DIR=$<TARGET_FILE_DIR:recomp-ui-psx-asset-stage-probe>/assets"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/asset_staging_test.cmake)

    # Compile the exact SDL_Renderer2 adapter through its public integration
    # helper. Calling the helper twice also guards its no-duplicate contract.
    find_package(SDL2 QUIET)
    if(TARGET SDL2::SDL2)
        add_library(recomp-runtime-ui-sdlrenderer2-check OBJECT)
        recomp_target_runtime_ui_sdlrenderer2(
            recomp-runtime-ui-sdlrenderer2-check)
        recomp_target_runtime_ui_sdlrenderer2(
            recomp-runtime-ui-sdlrenderer2-check)
        target_link_libraries(recomp-runtime-ui-sdlrenderer2-check PRIVATE
            SDL2::SDL2)
    endif()

    add_executable(recomp-runtime-ui-test
        tests/runtime_ui_test.c
        src/common/recomp_runtime_ui.c
        src/common/recomp_runtime_settings.c)
    target_include_directories(recomp-runtime-ui-test PRIVATE src)
    add_test(NAME recomp-runtime-ui COMMAND $<TARGET_FILE:recomp-runtime-ui-test>)

    # N64 controller page: the 3-column reading order the page is drawn from,
    # and the per-GUID gamepad store behind its bind chips.
    add_executable(recomp-ui-n64-pad-binds-test
        tests/n64_pad_binds_test.c
        src/consoles/n64/n64_pad_binds.c
        src/common/pad_binds.c)
    target_include_directories(recomp-ui-n64-pad-binds-test PRIVATE
        src src/common src/consoles/n64)
    target_link_libraries(recomp-ui-n64-pad-binds-test PRIVATE ${RUI_SDL_TARGET})
    target_compile_definitions(recomp-ui-n64-pad-binds-test PRIVATE ${RUI_SDL_DEFS})
    add_test(
        NAME recomp-ui-n64-pad-binds
        COMMAND $<TARGET_FILE:recomp-ui-n64-pad-binds-test>
                "${CMAKE_CURRENT_BINARY_DIR}")

    add_executable(recomp-ui-psx-binds-test
        tests/psx_binds_test.c
        src/consoles/psx/psx_binds.c)
    target_include_directories(recomp-ui-psx-binds-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-psx-binds-test PRIVATE ${RUI_SDL_TARGET})
    target_compile_definitions(recomp-ui-psx-binds-test PRIVATE ${RUI_SDL_DEFS})
    add_test(
        NAME recomp-ui-psx-binds
        COMMAND $<TARGET_FILE:recomp-ui-psx-binds-test>
                "${CMAKE_CURRENT_BINARY_DIR}/psx-binds-test.ini")

    # Multi-disc roster: sibling autofill + per-slot binding. Includes
    # launcher_model.c (not links it) to reach the static path rewriter, so the
    # model's own deps are compiled in alongside.
    add_executable(recomp-ui-launcher-discs-test
        tests/launcher_discs_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-discs-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-discs-test PRIVATE ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-launcher-discs
        COMMAND $<TARGET_FILE:recomp-ui-launcher-discs-test>
                "${CMAKE_CURRENT_BINARY_DIR}")

    # Renderer / VSync as enumerable lists rather than cycle buttons. Same
    # include-the-TU shape as the discs test above.
    add_executable(recomp-ui-launcher-video-pick-test
        tests/launcher_video_pick_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-video-pick-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-video-pick-test PRIVATE ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-launcher-video-pick
        COMMAND $<TARGET_FILE:recomp-ui-launcher-video-pick-test>)

    # Internal resolution as a host-supplied list (PSX presets), with the
    # legacy Supersampling cycle untouched when no vocabulary is supplied.
    add_executable(recomp-ui-launcher-internal-resolution-test
        tests/launcher_internal_resolution_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-internal-resolution-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-internal-resolution-test PRIVATE ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-launcher-internal-resolution
        COMMAND $<TARGET_FILE:recomp-ui-launcher-internal-resolution-test>)

    # First-run wizard: staging a retail BIOS that has no linked backend.
    # Same include-the-TU shape as the discs test above.
    add_executable(recomp-ui-launcher-setup-bios-test
        tests/launcher_setup_bios_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-setup-bios-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-setup-bios-test PRIVATE ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-launcher-setup-bios
        COMMAND $<TARGET_FILE:recomp-ui-launcher-setup-bios-test>
                "${CMAKE_CURRENT_BINARY_DIR}")

    # First-run wizard: Confirm disc / Continue records the pick immediately
    # (sidecars + host persist_setup), so the next start does not ask again.
    # Same include-the-TU shape as the setup-bios test above.
    add_executable(recomp-ui-launcher-setup-confirm-test
        tests/launcher_setup_confirm_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-setup-confirm-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-setup-confirm-test PRIVATE ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-launcher-setup-confirm
        COMMAND $<TARGET_FILE:recomp-ui-launcher-setup-confirm-test>
                "${CMAKE_CURRENT_BINARY_DIR}/setup-confirm-test")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/setup-confirm-test")

    # Per-seat pad_mode resolution: a locked mode (game.toml lock_mode) must
    # survive the keyboard-has-no-sticks presentation default, because the
    # selector that could undo it is hidden. Same include-the-TU shape as the
    # two tests above.
    add_executable(recomp-ui-launcher-pad-mode-test
        tests/launcher_pad_mode_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-pad-mode-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-pad-mode-test PRIVATE
        ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-launcher-pad-mode
        COMMAND $<TARGET_FILE:recomp-ui-launcher-pad-mode-test>)

    # Genesis seat ceiling: four logical players for a four-player title,
    # two for every two-port title, never more than the bind store routes.
    add_executable(recomp-ui-genesis-seats-test
        tests/genesis_seats_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-genesis-seats-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-genesis-seats-test PRIVATE
        ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-genesis-seats
        COMMAND $<TARGET_FILE:recomp-ui-genesis-seats-test>)

    # Netplay room seat ceiling (backend create_max_slots) is independent of
    # the local controller cards: a GBA link title seats four online, two on
    # LAN, and still draws one card. Plus dense bus positions from sparse
    # lobby seats (recomp_launcher_netplay_dense_position).
    add_executable(recomp-ui-netplay-seat-cap-test
        tests/netplay_seat_cap_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-netplay-seat-cap-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-netplay-seat-cap-test PRIVATE
        ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-netplay-seat-cap
        COMMAND $<TARGET_FILE:recomp-ui-netplay-seat-cap-test>)

    # Memory-card slot facts the Save panel paints: block-usage source order
    # (real inspect beats placeholder; a wired inspector never shows a made-up
    # pattern) and the tri-state enabled flag. Includes launcher_model.c like
    # the discs test to reach the static inspect helper.
    add_executable(recomp-ui-launcher-memcard-test
        tests/launcher_memcard_test.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-memcard-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-memcard-test PRIVATE ${RUI_SDL_TARGET})
    add_test(
        NAME recomp-ui-launcher-memcard
        COMMAND $<TARGET_FILE:recomp-ui-launcher-memcard-test>)

    # SNES first-launch input defaults: a gamepad slot with no GUID binds the
    # live pad (so the Input source box names it instead of reading "Gamepad"),
    # and a pad with no saved profile takes the console default deadzone.
    # Includes launcher_binds.c and links the model for the real set_source.
    add_executable(recomp-ui-snes-pad-defaults-test
        tests/snes_pad_defaults_test.c
        src/common/launcher_model.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-snes-pad-defaults-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-snes-pad-defaults-test PRIVATE ${RUI_SDL_TARGET})
    target_compile_definitions(recomp-ui-snes-pad-defaults-test PRIVATE ${RUI_SDL_DEFS})
    add_test(
        NAME recomp-ui-snes-pad-defaults
        COMMAND $<TARGET_FILE:recomp-ui-snes-pad-defaults-test>
                "${CMAKE_CURRENT_BINARY_DIR}")

    add_executable(recomp-ui-psx-profile-test
        tests/psx_profile_test.c)
    target_include_directories(recomp-ui-psx-profile-test PRIVATE
        src src/common src/consoles/psx)
    add_test(
        NAME recomp-ui-psx-profile
        COMMAND $<TARGET_FILE:recomp-ui-psx-profile-test>)

    add_executable(recomp-ui-nds-profile-test
        tests/nds_profile_test.c)
    target_include_directories(recomp-ui-nds-profile-test PRIVATE
        src src/common src/consoles/nds)
    add_test(
        NAME recomp-ui-nds-profile
        COMMAND $<TARGET_FILE:recomp-ui-nds-profile-test>)

    add_executable(recomp-ui-assist-pad-capture-test
        tests/launcher_assist_pad_capture_test.c
        src/common/launcher_model.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-assist-pad-capture-test PRIVATE
        src src/common src/consoles/psx)
    add_test(NAME recomp-ui-assist-pad-capture
             COMMAND $<TARGET_FILE:recomp-ui-assist-pad-capture-test>)

    # The shared presentation blend, including the holding variant a host with
    # a decoupled presentation clock needs. Pure arithmetic on two buffers, so
    # no SDL and no window.
    add_executable(recomp-ui-frame-blend-test
        tests/frame_blend_test.c
        src/common/recomp_frame_blend.c)
    target_include_directories(recomp-ui-frame-blend-test PRIVATE
        src src/common)
    add_test(NAME recomp-ui-frame-blend
             COMMAND $<TARGET_FILE:recomp-ui-frame-blend-test>)

    # Backspace in a launcher text field must not steal the focus ring: the
    # footer's Back/Start "re-home to PLAY" shortcut fired while an InputText
    # was active. Real ImGui frames, headless, same shape as the runtime-ui
    # test below.
    add_executable(recomp-ui-launcher-nav-backspace-test
        tests/launcher_nav_backspace_test.cpp
        src/third_party/imgui/imgui.cpp
        src/third_party/imgui/imgui_draw.cpp
        src/third_party/imgui/imgui_tables.cpp
        src/third_party/imgui/imgui_widgets.cpp)
    target_include_directories(recomp-ui-launcher-nav-backspace-test PRIVATE
        src src/common src/third_party/imgui)
    add_test(NAME recomp-ui-launcher-nav-backspace
             COMMAND $<TARGET_FILE:recomp-ui-launcher-nav-backspace-test>)

    # Hidden mod features: the default rule (listed while enabled) and the
    # title opt-in hide_hidden_features (never presented, package omitted).
    # Pure logic over a fake provider -- no SDL, no ImGui.
    add_executable(recomp-ui-launcher-mod-visibility-test
        tests/launcher_mod_visibility_test.c
        src/common/launcher_mod_visibility.c)
    target_include_directories(recomp-ui-launcher-mod-visibility-test PRIVATE
        src src/common)
    add_test(NAME recomp-ui-launcher-mod-visibility
             COMMAND $<TARGET_FILE:recomp-ui-launcher-mod-visibility-test>)

    add_executable(recomp-ui-launcher-model-cache-test
        tests/launcher_model_cache_test.c
        src/common/launcher_model.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    add_executable(recomp-ui-netplay-view-test
        tests/netplay_view_test.c src/common/launcher_model.c
        src/common/crc32.c src/common/sha1.c src/common/sha256.c
        src/common/ips_patch.c src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-netplay-view-test PRIVATE src src/common src/consoles/psx)
    add_test(NAME recomp-ui-netplay-view COMMAND $<TARGET_FILE:recomp-ui-netplay-view-test>)
    target_include_directories(recomp-ui-launcher-model-cache-test PRIVATE
        src src/common src/consoles/psx)
    # Readers also probe beside their executable. Keep them out of the setup
    # tests' executable directory, where those tests intentionally write caches.
    set_target_properties(recomp-ui-launcher-model-cache-test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher-model-cache-test")
    add_test(
        NAME recomp-ui-launcher-model-cache
        COMMAND $<TARGET_FILE:recomp-ui-launcher-model-cache-test>
                "${CMAKE_CURRENT_BINARY_DIR}/launcher-model-cache-test")

    add_executable(recomp-ui-launcher-rom-persistence-test
        tests/launcher_rom_persistence_test.c
        src/common/launcher_model.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-rom-persistence-test PRIVATE
        src src/common src/consoles/psx)
    set_target_properties(recomp-ui-launcher-rom-persistence-test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher-rom-persistence-test")
    add_test(NAME recomp-ui-launcher-rom-persistence
             COMMAND $<TARGET_FILE:recomp-ui-launcher-rom-persistence-test>
                     "${CMAKE_CURRENT_BINARY_DIR}/launcher-rom-persistence-test")
    set_tests_properties(recomp-ui-launcher-rom-persistence PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher-rom-persistence-test")

    add_executable(recomp-ui-launcher-in-session-test
        tests/launcher_in_session_test.c
        src/common/launcher_model.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-in-session-test PRIVATE
        src src/common src/consoles/psx)
    set_target_properties(recomp-ui-launcher-in-session-test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher-in-session-test")
    add_test(NAME recomp-ui-launcher-in-session
             COMMAND $<TARGET_FILE:recomp-ui-launcher-in-session-test>)
    set_tests_properties(recomp-ui-launcher-in-session PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher-in-session-test")

    add_executable(recomp-ui-launcher-settings-test
        tests/launcher_settings_test.c
        src/common/launcher_ng_capi.c
        src/common/launcher_model.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-settings-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-settings-test PRIVATE ${RUI_SDL_TARGET})
    target_compile_definitions(recomp-ui-launcher-settings-test PRIVATE ${RUI_SDL_DEFS})
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/settings-fixtures")
    add_test(NAME recomp-ui-launcher-settings
        COMMAND $<TARGET_FILE:recomp-ui-launcher-settings-test>
                "${CMAKE_CURRENT_BINARY_DIR}/settings-fixtures")

    # RECOMP_NETPLAY_LAUNCH: a match the Retro hub negotiated, handed to the
    # game's ingest_launch without opening the window.
    add_executable(recomp-ui-launcher-netplay-handoff-test
        tests/launcher_netplay_handoff_test.c
        src/common/launcher_ng_capi.c
        src/common/launcher_model.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c
        src/consoles/psx/memcard_format.c)
    target_include_directories(recomp-ui-launcher-netplay-handoff-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-launcher-netplay-handoff-test PRIVATE ${RUI_SDL_TARGET})
    target_compile_definitions(recomp-ui-launcher-netplay-handoff-test PRIVATE ${RUI_SDL_DEFS})
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/netplay-handoff")
    add_test(NAME recomp-ui-launcher-netplay-handoff
        COMMAND $<TARGET_FILE:recomp-ui-launcher-netplay-handoff-test>
                "${CMAKE_CURRENT_BINARY_DIR}/netplay-handoff")

    # A host-owned binding store under the NES profile leaves the runner's
    # keybinds.ini alone (launcher_binds.c nes_native_store).
    add_executable(recomp-ui-nes-host-bindings-test
        tests/nes_host_bindings_test.c
        src/common/launcher_model.c
        src/common/launcher_binds.c
        src/common/keybinds.c
        src/common/pad_binds.c
        src/consoles/nes/nes_binds.c
        src/consoles/psx/psx_binds.c
        src/consoles/psx/psx_pad_binds.c
        src/consoles/snes/snes_pad_binds.c
        src/consoles/n64/n64_binds.c
        src/consoles/n64/n64_pad_binds.c
        src/consoles/genesis/genesis_binds.c
        src/consoles/gb/gb_binds.c
        src/consoles/psx/memcard_format.c
        src/common/crc32.c
        src/common/sha1.c
        src/common/sha256.c
        src/common/ips_patch.c)
    target_include_directories(recomp-ui-nes-host-bindings-test PRIVATE
        src src/common src/consoles/psx)
    target_link_libraries(recomp-ui-nes-host-bindings-test PRIVATE ${RUI_SDL_TARGET})
    target_compile_definitions(recomp-ui-nes-host-bindings-test PRIVATE ${RUI_SDL_DEFS})
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/nes-host-bindings")
    add_test(NAME recomp-ui-nes-host-bindings
        COMMAND $<TARGET_FILE:recomp-ui-nes-host-bindings-test>
                "${CMAKE_CURRENT_BINARY_DIR}/nes-host-bindings")

    add_executable(recomp-runtime-ui-imgui-test
        tests/runtime_ui_imgui_test.cpp
        src/common/recomp_runtime_ui.c
        src/common/recomp_runtime_settings.c
        src/common/backends/imgui/runtime_ui_imgui.cpp
        src/third_party/imgui/imgui.cpp
        src/third_party/imgui/imgui_draw.cpp
        src/third_party/imgui/imgui_tables.cpp
        src/third_party/imgui/imgui_widgets.cpp)
    target_include_directories(recomp-runtime-ui-imgui-test PRIVATE
        src src/common src/third_party/imgui)
    add_test(NAME recomp-runtime-ui-imgui COMMAND $<TARGET_FILE:recomp-runtime-ui-imgui-test>)

    # The optional netplay backend (recomp_target_launcher_netplay). Built and
    # tested only when a recomp-net checkout is named; recomp-ui itself never
    # needs one.
    #   cmake -DRECOMP_UI_RECOMP_NET_DIR=<path-to-recomp-net> ...
    set(RECOMP_UI_RECOMP_NET_DIR "" CACHE PATH
        "recomp-net checkout for the optional netplay backend's test")
    if(RECOMP_UI_RECOMP_NET_DIR AND NOT WIN32)
        set(RNET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(RNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${RECOMP_UI_RECOMP_NET_DIR}"
                         "${CMAKE_BINARY_DIR}/recomp-net-build" EXCLUDE_FROM_ALL)
        add_executable(recomp-ui-netplay-host-test tests/netplay_host_test.c)
        target_include_directories(recomp-ui-netplay-host-test PRIVATE src)
        find_package(Threads REQUIRED)
        target_link_libraries(recomp-ui-netplay-host-test PRIVATE recomp_net
                              Threads::Threads)
        target_compile_definitions(recomp-ui-netplay-host-test PRIVATE
            _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
        add_test(NAME recomp-ui-netplay-host
                 COMMAND $<TARGET_FILE:recomp-ui-netplay-host-test>)
    endif()
endif()
