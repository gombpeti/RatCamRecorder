# Stage the pylon x64 runtime DLLs beside our executables.
#
# Not optional politeness -- without this the binaries do not start outside a
# shell that happens to have the right PATH. On this rig the persisted system
# PATH contains ONLY:
#
#   C:\Program Files\Basler\pylon\Runtime\Win32\
#
# so Explorer resolves PylonBase_v12.dll and friends to the 32-bit build and a
# 64-bit process dies with 0xc000007b (STATUS_INVALID_IMAGE_FORMAT) before
# main() is ever reached. The exe's own directory is searched first, so copying
# the x64 DLLs there makes the tools independent of ambient PATH.
#
# Only the root DLLs are copied (~39 MB). pylon locates its transport-layer
# plugins (.cti) itself through its registry keys, so those are left alone.

set(PYLON_RUNTIME_DIR "C:/Program Files/Basler/pylon/Runtime/x64" CACHE PATH
    "pylon x64 runtime DLL directory")

if(NOT EXISTS "${PYLON_RUNTIME_DIR}")
  message(WARNING "pylon x64 runtime not found at ${PYLON_RUNTIME_DIR}; "
                  "executables may fail to start with 0xc000007b.")
  set(PYLON_RUNTIME_DLLS "")
else()
  file(GLOB PYLON_RUNTIME_DLLS "${PYLON_RUNTIME_DIR}/*.dll")
  list(LENGTH PYLON_RUNTIME_DLLS _n)
  message(STATUS "pylon runtime: ${PYLON_RUNTIME_DIR} (${_n} DLLs)")
endif()

function(campy_copy_pylon_dlls target)
  foreach(_dll IN LISTS PYLON_RUNTIME_DLLS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_dll}" "$<TARGET_FILE_DIR:${target}>")
  endforeach()
endfunction()
