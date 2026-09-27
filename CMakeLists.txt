cmake_minimum_required(VERSION 3.16)

# ESP-IDF expects a 'main' component by default; our sources live in 'src/'
set(EXTRA_COMPONENT_DIRS src)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voxedge_kws)
