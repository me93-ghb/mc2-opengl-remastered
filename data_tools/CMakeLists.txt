# LINK-CONFIG-FIX: a few game-layer globals (eye, MaxMinUV, ObjectManager,
# g_tobjAngularCyc, RunGameplayPickSelfTest) are referenced by the renderer/mclib
# object files but defined only in code/ TUs the tools don't compile. Provide
# trivial definitions via a shared stub TU rather than linking the whole game.
set(DATA_TOOLS_STUBS "data_tools_game_stubs.cpp")

set(MAKEFST_SOURCES "makefst.cpp" ${DATA_TOOLS_STUBS})
set(PAK_SOURCES "pak.cpp" "common.hpp" ${DATA_TOOLS_STUBS})
set(ASECONV_SOURCES "aseconv.cpp" "common.hpp" ${DATA_TOOLS_STUBS})
set(MAKERSP_SOURCES "makersp.cpp" ${DATA_TOOLS_STUBS})

add_compile_definitions(DISABLE_GAMEOS_MAIN)

# LINK-CONFIG-FIX: these host tools link the full `gameos` library, which over
# time grew TUs that reference the renderer libs (MLR / gosFX / RenderWorld /
# RenderCore / GameAdapters / static-prop batcher). gameos declares those only
# as PRIVATE deps, so any consumer must link the same closure mc2 does or the
# link fails with ~200 unresolved renderer externals. Mirror mc2's engine lib
# set here (the tools pull in the renderer purely to satisfy gameos's symbol
# references; the offline code paths never call into it). GL/GLEW are required
# because the renderer TUs reference them.
set(DATA_TOOLS_ENGINE_LIBS
    mclib
    gameadapters
    renderworld
    rendercore
    editorbridge
    gosfx
    mlr
    particles
    fx_trace
    stuff
    gameos
    windows
    ZLIB::ZLIB
    ${SDL2_LIBRARIES}
    GLEW::GLEW
    SDL2_mixer::SDL2_mixer
    ${ADDITIONAL_LIBS}
    # macos-port: gos_render.cpp calls Mesa's EGL/GL directly; same libs as mc2
    # (empty off Apple).
    ${MC2_MACOS_MESA_GL}
    ${MC2_MACOS_MESA_EGL}
    OpenGL::GL
)

add_executable(makefst ${MAKEFST_SOURCES})
target_link_libraries(makefst ${DATA_TOOLS_ENGINE_LIBS})

add_executable(pak ${PAK_SOURCES})
target_link_libraries(pak ${DATA_TOOLS_ENGINE_LIBS})

add_executable(aseconv ${ASECONV_SOURCES})
# aseconv.cpp already defines `eye` + `MaxMinUV`; suppress the stub's copies.
target_compile_definitions(aseconv PRIVATE DATA_TOOLS_STUBS_HAVE_EYE_AND_MAXMINUV)
target_link_libraries(aseconv ${DATA_TOOLS_ENGINE_LIBS})

add_executable(makersp ${MAKERSP_SOURCES})
target_link_libraries(makersp ${DATA_TOOLS_ENGINE_LIBS})

