# Locates the vendored FFmpeg LGPL shared build under
# 3rdparty/ffmpeg-lgpl-win64/. Exposes one IMPORTED target per library:
#   FFmpegLGPL::avcodec
#   FFmpegLGPL::avformat
#   FFmpegLGPL::avutil
#   FFmpegLGPL::swscale
#   FFmpegLGPL::swresample
# Plus FFmpegLGPL_RUNTIME_DLLS (list of absolute DLL paths) for install().

if(APPLE)
    # macos-port: the vendored tree is Windows-only (.dll + .lib). Use the system
    # / Homebrew FFmpeg (.dylib) instead, exposing the same FFmpegLGPL::* imported
    # targets so nothing else in the build has to change.
    find_path(FFmpegLGPL_INCLUDE_DIR libavcodec/avcodec.h
        PATHS /opt/homebrew/include /usr/local/include)
    if(NOT FFmpegLGPL_INCLUDE_DIR)
        message(FATAL_ERROR "FFmpegLGPL: FFmpeg headers not found. Run: brew install ffmpeg")
    endif()
    set(FFmpegLGPL_RUNTIME_DLLS "")
    foreach(_n avcodec avformat avutil swscale swresample)
        find_library(_ff_lib_${_n} ${_n} PATHS /opt/homebrew/lib /usr/local/lib)
        if(NOT _ff_lib_${_n})
            message(FATAL_ERROR "FFmpegLGPL: lib${_n} not found. Run: brew install ffmpeg")
        endif()
        add_library(FFmpegLGPL::${_n} SHARED IMPORTED)
        set_target_properties(FFmpegLGPL::${_n} PROPERTIES
            IMPORTED_LOCATION "${_ff_lib_${_n}}"
            INTERFACE_INCLUDE_DIRECTORIES "${FFmpegLGPL_INCLUDE_DIR}")
    endforeach()
    set(FFmpegLGPL_FOUND TRUE)
    # No DLLs to sideload on macOS (dylibs are linked directly) -> empty name list.
    set(FFMPEG_DLL_NAMES_CLIST "")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/mc2video_dlls.h.in"
        "${CMAKE_BINARY_DIR}/generated/mc2video_dlls.h"
        @ONLY)
    message(STATUS "FFmpegLGPL (macos-port): ${FFmpegLGPL_INCLUDE_DIR} + Homebrew dylibs")
    return()
endif()

set(FFMPEG_VENDOR_DIR "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg-lgpl-win64")

if(NOT EXISTS "${FFMPEG_VENDOR_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "FFmpegLGPL: vendored tree not found at ${FFMPEG_VENDOR_DIR}. "
        "Run the video-playback plan's Task 3 to populate it.")
endif()

set(FFmpegLGPL_INCLUDE_DIR "${FFMPEG_VENDOR_DIR}/include")

function(_ffmpeg_add_lib NAME)
    file(GLOB _imp_lib "${FFMPEG_VENDOR_DIR}/lib/${NAME}.lib")
    file(GLOB _dll "${FFMPEG_VENDOR_DIR}/bin/${NAME}-*.dll")
    if(NOT _imp_lib OR NOT _dll)
        message(FATAL_ERROR
            "FFmpegLGPL: could not find ${NAME} import lib or DLL in ${FFMPEG_VENDOR_DIR}")
    endif()
    add_library(FFmpegLGPL::${NAME} SHARED IMPORTED)
    set_target_properties(FFmpegLGPL::${NAME} PROPERTIES
        IMPORTED_LOCATION "${_dll}"
        IMPORTED_IMPLIB   "${_imp_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFmpegLGPL_INCLUDE_DIR}")
    get_filename_component(_dll_name "${_dll}" NAME)
    list(APPEND FFmpegLGPL_RUNTIME_DLLS "${_dll}")
    set(FFmpegLGPL_RUNTIME_DLLS "${FFmpegLGPL_RUNTIME_DLLS}" PARENT_SCOPE)
    set(FFmpegLGPL_${NAME}_DLL_NAME "${_dll_name}" PARENT_SCOPE)
endfunction()

set(FFmpegLGPL_RUNTIME_DLLS "")
_ffmpeg_add_lib(avcodec)
_ffmpeg_add_lib(avformat)
_ffmpeg_add_lib(avutil)
_ffmpeg_add_lib(swscale)
_ffmpeg_add_lib(swresample)

set(FFmpegLGPL_FOUND TRUE)
message(STATUS "FFmpegLGPL: ${FFMPEG_VENDOR_DIR}")
message(STATUS "FFmpegLGPL DLLs: ${FFmpegLGPL_RUNTIME_DLLS}")

# Generate a C header listing the runtime DLL filenames so
# ffmpegProbeAvailability() in mc2video.cpp can enumerate them
# without hardcoding. Single source of truth: the vendored tree.
set(_dll_names_c "")
foreach(_dll_path ${FFmpegLGPL_RUNTIME_DLLS})
    get_filename_component(_n "${_dll_path}" NAME)
    string(APPEND _dll_names_c "    \"${_n}\",\n")
endforeach()
set(FFMPEG_DLL_NAMES_CLIST "${_dll_names_c}")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/mc2video_dlls.h.in"
    "${CMAKE_BINARY_DIR}/generated/mc2video_dlls.h"
    @ONLY)
