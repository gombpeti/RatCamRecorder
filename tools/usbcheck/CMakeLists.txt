add_executable(usbcheck main.cpp)
target_link_libraries(usbcheck PRIVATE pylon::pylon)
campy_copy_pylon_dlls(usbcheck)
