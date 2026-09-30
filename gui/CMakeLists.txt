# No WIN32: keep a console. CameraWorker and VideoEncoder report through
# printf, and a lab tool that hides its own diagnostics is worse than one
# with an extra window.
# The icon is optional: build without it until the artwork is in place, rather
# than failing the build over a missing picture.
set(RATCAM_ICON_SOURCES)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources/ratcam.ico)
  list(APPEND RATCAM_ICON_SOURCES resources/RatCamRecorder.rc)
  message(STATUS "Icon: resources/ratcam.ico")
else()
  message(STATUS "Icon: resources/ratcam.ico not found -- run gui/make_icon.py")
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources/ratcam.png)
  list(APPEND RATCAM_ICON_SOURCES resources/ratcam.qrc)
endif()

qt_add_executable(RatCamRecorder
  main.cpp MainWindow.cpp MainWindow.h
  PreviewGrid.cpp PreviewGrid.h PreviewWindow.cpp PreviewWindow.h
  PreflightDialog.cpp PreflightDialog.h
  ${RATCAM_ICON_SOURCES})
target_link_libraries(RatCamRecorder PRIVATE
  campy_core
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::SerialPort
)
campy_copy_ffmpeg_dlls(RatCamRecorder)
campy_copy_pylon_dlls(RatCamRecorder)

# Qt's own DLLs next to the binary, so it starts without Qt on PATH.
if(WIN32)
  find_program(QT_DEPLOY_EXE windeployqt HINTS ${QT_ROOT}/bin)
  if(QT_DEPLOY_EXE)
    add_custom_command(TARGET RatCamRecorder POST_BUILD
      COMMAND ${QT_DEPLOY_EXE} --no-translations --no-system-d3d-compiler
              --no-opengl-sw "$<TARGET_FILE:RatCamRecorder>"
      COMMENT "Deploying Qt runtime")
  endif()
endif()
