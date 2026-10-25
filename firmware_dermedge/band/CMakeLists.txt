# DermEdge band: ADXL345 scratch detection, touch pad, ST7735 screen and a Bluetooth link to the
# website, on a plain ESP32. Host tests for the logic: sh test/run.sh
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dermedge_band)
