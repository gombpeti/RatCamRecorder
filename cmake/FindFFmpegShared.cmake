# Locate a prebuilt FFmpeg "shared" package (BtbN / gyan.dev layout):
#
#   <root>/include/libavcodec/avcodec.h
#   <root>/lib/avcodec.lib ...
#   <root>/bin/avcodec-*.dll ...
#
# These builds are compiled with MinGW, which is fine: FFmpeg is pure C and the
# C ABI is stable, so MSVC links against the import libraries without trouble.
# (The same is NOT true of a C++ library such as pylon, which is why that needs
# a toolset-matched VC142 build.)
#
# Defines the imported target ffmpeg::ffmpeg and FFMPEG_RUNTIME_DLLS.

set(FFMPEG_ROOT "C:/Qt/ffmpeg-shared" CACHE PATH
    "Root of a prebuilt FFmpeg shared package containing include/, lib/ and bin/")

set(_ffmpeg_libs avcodec avformat avutil swscale swresample)

if(NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
  message(FATAL_ERROR
    "FFmpeg headers not found under ${FFMPEG_ROOT}/include.\n"
    "Point -DFFMPEG_ROOT=... at a *shared* prebuilt package (one with include/, "
    "lib/ and bin/). A source tarball will not do -- it has no compiled libs.")
endif()

add_library(ffmpeg::ffmpeg INTERFACE IMPORTED)
target_include_directories(ffmpeg::ffmpeg INTERFACE "${FFMPEG_ROOT}/include")

foreach(_lib IN LISTS _ffmpeg_libs)
  set(_path "${FFMPEG_ROOT}/lib/${_lib}.lib")
  if(NOT EXISTS "${_path}")
    message(FATAL_ERROR "Missing ${_path}. Is this a shared (not static) build?")
  endif()
  target_link_libraries(ffmpeg::ffmpeg INTERFACE "${_path}")
endforeach()

# The DLLs must sit beside the executable at run time.
file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_ROOT}/bin/*.dll")
if(NOT FFMPEG_RUNTIME_DLLS)
  message(WARNING "No DLLs in ${FFMPEG_ROOT}/bin -- executables will not start.")
endif()

message(STATUS "FFmpeg: ${FFMPEG_ROOT} (${_ffmpeg_libs})")

# Copy the runtime DLLs next to a target's binary after it builds.
function(campy_copy_ffmpeg_dlls target)
  foreach(_dll IN LISTS FFMPEG_RUNTIME_DLLS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_dll}" "$<TARGET_FILE_DIR:${target}>")
  endforeach()
endfunction()
