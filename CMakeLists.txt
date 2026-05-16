# Top-level CMakeLists for the lamp firmware.
# Invoked indirectly by `make build` (see Makefile + ESP-IDF's idf.py).

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lamp)
