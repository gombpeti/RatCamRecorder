add_executable(camfeatures main.cpp)
target_link_libraries(camfeatures PRIVATE pylon::pylon)
campy_copy_pylon_dlls(camfeatures)
