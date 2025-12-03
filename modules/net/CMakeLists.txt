cmake_minimum_required(VERSION 3.10)

add_library(recum12_net
    src/RemoteProto.cpp
    src/BlynkClient.cpp
    src/CommandDispatcher.cpp
    src/LineTcpClient.cpp
)

target_include_directories(recum12_net
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
