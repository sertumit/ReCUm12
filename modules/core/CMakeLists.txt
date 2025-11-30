cmake_minimum_required(VERSION 3.10)

add_library(recum12_core
    src/PumpRuntimeState.cpp
    src/PumpSaleTracker.cpp
    src/UserManager.cpp
    src/RfidAuthController.cpp
)

target_include_directories(recum12_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(recum12_core
    PUBLIC
        recum12_hw
        recum12_rfid
)