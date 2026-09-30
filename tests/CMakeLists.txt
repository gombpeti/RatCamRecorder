add_executable(test_ringbuffer test_ringbuffer.cpp)
target_include_directories(test_ringbuffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME ringbuffer COMMAND test_ringbuffer)

add_executable(test_encoder test_encoder.cpp)
target_link_libraries(test_encoder PRIVATE campy_core)
campy_copy_ffmpeg_dlls(test_encoder)
add_test(NAME encoder COMMAND test_encoder)
