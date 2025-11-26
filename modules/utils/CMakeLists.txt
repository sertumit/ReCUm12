cmake_minimum_required(VERSION 3.10)

add_library(recum12_utils
    src/LogManager.cpp
    src/Settings.cpp
)

target_include_directories(recum12_utils
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
