cmake_minimum_required(VERSION 3.16)
project(dslab C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(ZLIB REQUIRED zlib)
find_package(Threads REQUIRED)

set(VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/DSView)

# ---- libsigrok4DSL + common (vendored from DSView @2e9e2c8e, see docs/upstream.md)
set(SIGROK4DSL_SOURCES
    ${VENDOR}/libsigrok4DSL/version.c
    ${VENDOR}/libsigrok4DSL/strutil.c
    ${VENDOR}/libsigrok4DSL/std.c
    ${VENDOR}/libsigrok4DSL/session_driver.c
    ${VENDOR}/libsigrok4DSL/session.c
    ${VENDOR}/libsigrok4DSL/log.c
    ${VENDOR}/libsigrok4DSL/hwdriver.c
    ${VENDOR}/libsigrok4DSL/error.c
    ${VENDOR}/libsigrok4DSL/backend.c
    ${VENDOR}/libsigrok4DSL/output/output.c
    ${VENDOR}/libsigrok4DSL/input/input.c
    ${VENDOR}/libsigrok4DSL/hardware/demo/demo.c
    ${VENDOR}/libsigrok4DSL/input/in_binary.c
    ${VENDOR}/libsigrok4DSL/input/in_vcd.c
    ${VENDOR}/libsigrok4DSL/input/in_wav.c
    ${VENDOR}/libsigrok4DSL/output/csv.c
    ${VENDOR}/libsigrok4DSL/output/gnuplot.c
    ${VENDOR}/libsigrok4DSL/output/srzip.c
    ${VENDOR}/libsigrok4DSL/output/vcd.c
    ${VENDOR}/libsigrok4DSL/hardware/DSL/dslogic.c
    ${VENDOR}/libsigrok4DSL/hardware/common/usb.c
    ${VENDOR}/libsigrok4DSL/hardware/common/ezusb.c
    ${VENDOR}/libsigrok4DSL/trigger.c
    ${VENDOR}/libsigrok4DSL/dsdevice.c
    ${VENDOR}/libsigrok4DSL/hardware/DSL/dscope.c
    ${VENDOR}/libsigrok4DSL/hardware/DSL/command.c
    ${VENDOR}/libsigrok4DSL/hardware/DSL/dsl.c
    ${VENDOR}/libsigrok4DSL/lib_main.c
    ${VENDOR}/common/minizip/zip.c
    ${VENDOR}/common/minizip/unzip.c
    ${VENDOR}/common/minizip/ioapi.c
    ${VENDOR}/common/log/xlog.c
)

add_library(sigrok4dsl STATIC ${SIGROK4DSL_SOURCES})
target_include_directories(sigrok4dsl PUBLIC
    ${VENDOR}/libsigrok4DSL
    ${VENDOR}/common
    ${GLIB_INCLUDE_DIRS}
    ${LIBUSB_INCLUDE_DIRS}
)
# See the comment at the dslab target: glib include dirs are exported so
# every consumer of libsigrok.h can find glib.h.

target_link_libraries(sigrok4dsl PUBLIC
    ${GLIB_LIBRARIES}
    ${LIBUSB_LIBRARIES}
    ${ZLIB_LIBRARIES}
    Threads::Threads
)
# DS firmware/FPGA resource dir (overridable at configure or run time via DSLAB_RES_DIR)
set(DSLAB_RES_DIR_DEFAULT "${VENDOR}/res" CACHE PATH "Default firmware/FPGA resource directory")

# ---- dslab CLI
add_executable(dslab
    src/dslab.c
    src/jsonw.c
    src/util.c
    src/usb_scan.c
    src/dsl_session.c
    src/capture.c
    src/trigger.c
    src/session_mgr.c
    src/unpack.c
)
target_compile_definitions(dslab PRIVATE DSLAB_RES_DIR_DEFAULT="${DSLAB_RES_DIR_DEFAULT}")
target_link_libraries(dslab PRIVATE sigrok4dsl)
# Ship the runtime DLLs next to the exe so it runs without MSYS2 on PATH.
if(WIN32)
  set(_dll_dirs "")
  foreach(_pfx LIBUSB GLIB ZLIB)
    list(APPEND _dll_dirs "${${_pfx}_LIBRARY_DIRS}/../bin")
  endforeach()
  foreach(_dll libusb-1.0.dll libglib-2.0-0.dll libpcre2-8-0.dll libintl-8.dll
                 libiconv-2.dll libwinpthread-1.dll zlib1.dll)
    find_file(_found_dll_${_dll} NAMES ${_dll} HINTS ${_dll_dirs} NO_DEFAULT_PATH)
    if(_found_dll_${_dll})
      add_custom_command(TARGET dslab POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_found_dll_${_dll}}" $<TARGET_FILE_DIR:dslab>)
    endif()
  endforeach()
endif()

# ---- unit tests
enable_testing()
add_executable(dslab_tests
    tests/unit/test_unpack.c
    tests/unit/test_util.c
    tests/unit/test_trigger.c
    src/unpack.c
    src/util.c
    src/trigger.c
)
target_include_directories(dslab_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME unit COMMAND dslab_tests)

# CLI JSON contract / replay tests (python), no hardware required
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME cli_contract
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_cli_contract.py $<TARGET_FILE:dslab>)
  add_test(NAME demo_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_demo_pipeline.py $<TARGET_FILE:dslab>)
  add_test(NAME async_lifecycle
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/test_async_lifecycle.py $<TARGET_FILE:dslab>)
endif()
