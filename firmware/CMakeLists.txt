# ESP32-S3-Touch-LCD-4.3C  race refuel countdown timer
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-fdiagnostics-color=always)
add_compile_options(-Wno-attributes)

project(fuel_stop_timer)
