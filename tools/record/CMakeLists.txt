add_executable(record main.cpp)
target_link_libraries(record PRIVATE campy_core)
campy_copy_ffmpeg_dlls(record)
campy_copy_pylon_dlls(record)
