cmake_minimum_required(VERSION 3.20)
project(campy_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

if(MSVC)
  add_compile_options(/W4 /MP /utf-8)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# pylon ships its own package config under Development/CMake/pylon
find_package(pylon 12 REQUIRED)
include(FindFFmpegShared)
include(PylonRuntime)

# Every executable into one directory, so the pylon / FFmpeg / Qt runtimes are
# staged once instead of per target.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Qt 6.5 LTS, MSVC 2019 kit -- must match the toolset pylon's VC142 libs need.
set(QT_ROOT "C:/Qt/6.5.12/msvc2019_64" CACHE PATH "Qt 6.5 LTS msvc2019_64 kit")
list(APPEND CMAKE_PREFIX_PATH ${QT_ROOT})
find_package(Qt6 6.5 COMPONENTS Core Gui Widgets SerialPort QUIET)
if(Qt6_FOUND)
  message(STATUS "Qt: ${QT_ROOT} (${Qt6_VERSION})")
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTOUIC ON)
  set(CMAKE_AUTORCC ON)
else()
  message(WARNING "Qt6 not found under ${QT_ROOT}; the GUI will not be built.")
endif()

# ---------------------------------------------------------------- packaging
# Everything the application needs at run time goes into one folder, so it can
# be installed or zipped without depending on PATH. The one thing that cannot
# be bundled is Basler's kernel driver (plnu3v): pylon must be installed on the
# target machine for the cameras to enumerate at all.
set(CMAKE_INSTALL_PREFIX "C:/Program Files/RatCam Recorder"
    CACHE PATH "Install location" FORCE)

install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/Release/"
        DESTINATION .
        FILES_MATCHING
          PATTERN "RatCamRecorder.exe"
          PATTERN "record.exe"
          PATTERN "usbcheck.exe"
          PATTERN "*.dll"
          PATTERN "platforms/*"
          PATTERN "styles/*"
          PATTERN "imageformats/*"
          PATTERN "iconengines/*"
          PATTERN "networkinformation/*"
          PATTERN "tls/*"
          PATTERN "generic/*")

install(FILES "${CMAKE_SOURCE_DIR}/gui/resources/ratcam.ico" DESTINATION .)
install(FILES "${CMAKE_SOURCE_DIR}/DESIGN.md" DESTINATION docs OPTIONAL)

set(CPACK_PACKAGE_NAME "RatCam Recorder")
set(CPACK_PACKAGE_VENDOR "Peter Gombkoto")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Synchronised multi-camera recorder for Basler cameras")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "RatCam Recorder")
set(CPACK_PACKAGE_EXECUTABLES "RatCamRecorder;RatCam Recorder")
set(CPACK_CREATE_DESKTOP_LINKS "RatCamRecorder")
set(CPACK_GENERATOR "ZIP")
include(CPack)

enable_testing()

add_subdirectory(src)
add_subdirectory(tools/usbcheck)
add_subdirectory(tools/record)
add_subdirectory(tools/camfeatures)
add_subdirectory(tests)
if(Qt6_FOUND)
  add_subdirectory(gui)
endif()
