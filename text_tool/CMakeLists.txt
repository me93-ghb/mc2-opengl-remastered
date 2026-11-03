add_definitions(-DUSE_ASSEMBLER_CODE=0)
add_definitions(-DLINUX_BUILD)


set(TEXT_TOOL_SOURCES "main.cpp")

include_directories(${CMAKE_PREFIX_PATH}/include)
add_executable(text_tool ${TEXT_TOOL_SOURCES})
# text_tool rasterizes fonts with SDL_ttf alone and opens no GL context, but it
# links gameos, which needs GL and GLEW. macos-port: as for mc2, Mesa's libGL goes
# ahead of OpenGL::GL (empty on non-Apple).
target_link_libraries(text_tool gameos windows ${SDL2_LIBRARIES} SDL2_ttf::SDL2_ttf GLEW::GLEW ${ADDITIONAL_LIBS} ${MC2_MACOS_MESA_GL} OpenGL::GL)
