# Core acquisition library. Split so the pylon-only parts stay testable
# independently of the encoder.
add_library(campy_core STATIC
  camera/CameraWorker.cpp
  core/Recorder.cpp
  trigger/SerialTrigger.cpp
  encode/VideoEncoder.cpp
  meta/MetadataWriter.cpp
)
target_include_directories(campy_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(campy_core PUBLIC pylon::pylon ffmpeg::ffmpeg)
