# SwitchView ESP32-S2 firmware: Pro Controller USB device driven over Wi-Fi.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(USB_LAYOUT "1" CACHE STRING "0 controller, 1 composite, 2 keyboard")
if(NOT USB_LAYOUT MATCHES "^[012]$")
  message(FATAL_ERROR "Invalid USB_LAYOUT")
endif()
add_compile_definitions(USB_LAYOUT=${USB_LAYOUT})

project(switchview_s2 CXX C)
