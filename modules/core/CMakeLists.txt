cmake_minimum_required(VERSION 3.10)

add_library(recum12_core
    src/PumpSaleTracker.cpp
    src/UserManager.cpp
)

target_include_directories(recum12_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
