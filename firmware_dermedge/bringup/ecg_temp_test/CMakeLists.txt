# DermEdge ECG + LM35 bring-up: streams the AD8232's ECG, its lead-off pins and the LM35 over serial.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dermedge_ecg_temp_test)
