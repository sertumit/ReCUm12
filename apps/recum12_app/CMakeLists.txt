cmake_minimum_required(VERSION 3.10)

add_executable(recum12_app
    src/main.cpp
)

target_link_libraries(recum12_app
    PRIVATE
        recum12_hw
        recum12_core
        recum12_gui
        recum12_rfid
        recum12_net
        recum12_utils
)
