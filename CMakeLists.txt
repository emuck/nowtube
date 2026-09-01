cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nowtube)

# LVGL's custom tick header is owned by the main component.  Use an absolute
# source path so fresh out-of-tree ESP-IDF builds work as well as in-tree ones.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_SOURCE_DIR}/main" APPEND)
