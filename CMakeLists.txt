cmake_minimum_required(VERSION 3.16)

add_compile_options(-Wno-error=missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(matrix-write)