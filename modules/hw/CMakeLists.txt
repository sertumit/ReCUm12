cmake_minimum_required(VERSION 3.10)

add_library(recum12_hw
    src/PumpInterfaceLvl3.cpp
    src/PumpR07Protocol.cpp
)

target_include_directories(recum12_hw
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
