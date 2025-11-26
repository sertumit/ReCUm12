cmake_minimum_required(VERSION 3.10)

add_library(recum12_rfid
    src/Pn532Reader.cpp
)

target_include_directories(recum12_rfid
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
