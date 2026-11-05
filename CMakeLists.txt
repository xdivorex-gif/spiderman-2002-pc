cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Spider-Man (2002, Xbox) - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(spiderman_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/gpu_backend.c       # D3D11 render back end (RECOMP_GPU_RENDER=1)
    src/settings.c          # settings.ini -> RECOMP_* switches
    src/spiderman.rc        # replacement art (PlayStation button prompts)
    src/text_patch.c        # Xbox wording in unpacked menu text -> PC
    src/costume_panel.c     # costume choice, and its panel over the pause menu
    src/shocker_input.c     # the Shocker's blasts on L2 + a face button
    src/shocker_powers.c    # his tornado's pull and damage, in the game
    src/subtitles.c         # transcribed voice lines and movies, on screen
    src/movie_upscale.c     # movies\NAME.mp4 shown for the Bink of that name
    src/movement.c          # earlier run, no dead spot on the left stick
    src/legal_pc.c          # the PC legal screen, made from the player's own copy
    src/shocker_heat.c      # his gauntlets' heat, the limit on his specials
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src/kernel    # nv2a_backend.h, for gpu_backend.c
    ${XBOXRECOMP_DIR}/src/usb       # usb_gamepad.h, for costume_panel.c
    ${XBOXRECOMP_DIR}/src/d3d       # d3d8_swizzle.h (format tests), for gpu_backend.c
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
    d3dcompiler             # gpu_backend.c compiles its shaders at start-up
    windowscodecs ole32     # gpu_backend.c reads texture-pack PNGs with WIC
    mfplat mfreadwrite mfuuid  # movie_upscale.c decodes the upscaled movies
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # build/Release/spiderman_recomp.map: function addresses, for mapping
        # RECOMP_SAMPLE's instruction-pointer samples back to names.
        /MAP
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

# The settings window in front of the game (src/launcher.c): edits
# settings.ini next to the game exe (src/settings.c reads it) and starts it.
add_executable(SpiderManLauncher WIN32 src/launcher.c src/launcher.rc)
target_link_libraries(SpiderManLauncher PRIVATE comctl32 msimg32)

# The game's text with PC wording (tools/make_overlay.py builds overlay/),
# beside the exe where settings.c points RECOMP_OVERLAY_DIR.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/overlay $<TARGET_FILE_DIR:${PROJECT_NAME}>/overlay)

# Hero models for the costume panel's 3D view (tools/xbxm.py --export builds
# previews/ from game_files; costume_panel.c reads them), beside the exe.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/previews)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/previews $<TARGET_FILE_DIR:${PROJECT_NAME}>/previews)
endif()

# High-res menu art and fonts (texpack/<texels hash>.png, gpu_backend.c),
# beside the exe.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/texpack $<TARGET_FILE_DIR:${PROJECT_NAME}>/texpack)
