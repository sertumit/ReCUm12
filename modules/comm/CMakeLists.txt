cmake_minimum_required(VERSION 3.10)

add_library(recum12_comm
    src/NetworkManager.cpp
)

target_include_directories(recum12_comm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
