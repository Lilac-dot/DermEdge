# DermEdge GSR wristband + skin moisture patch on a plain ESP32.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ../../firmware_dermedge/components)  # archived here from firmware_dermedge/wristband
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dermedge_wristband)
