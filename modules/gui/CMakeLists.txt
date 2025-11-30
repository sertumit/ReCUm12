cmake_minimum_required(VERSION 3.10)

add_library(recum12_gui
    src/MainWindow.cpp
    src/rs485_gui_adapter.cpp
    src/StatusMessageController.cpp
)

target_include_directories(recum12_gui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${GTKMM_INCLUDE_DIRS}
)

target_link_libraries(recum12_gui
    PUBLIC
        ${GTKMM_LIBRARIES}
    PRIVATE
        recum12_core
)