add_definitions(-DUSE_ASSEMBLER_CODE=0)
add_definitions(-DLINUX_BUILD)


set(TEXT_TOOL_SOURCES "main.cpp")

include_directories(${CMAKE_PREFIX_PATH}/include)
add_executable(text_tool ${TEXT_TOOL_SOURCES})
# macos-port: text_tool rasterizes fonts through gameos's GL 4.3 context, so it
# hits the same Apple-GL-4.1 wall as mc2. Link Mesa's libGL ahead of OpenGL::GL
# (empty on non-Apple) and run under dev/macos-run.sh's Zink env + glshim.
target_link_libraries(text_tool gameos windows ${SDL2_LIBRARIES} SDL2_ttf::SDL2_ttf GLEW::GLEW ${ADDITIONAL_LIBS} ${MC2_MACOS_MESA_GL} OpenGL::GL)
