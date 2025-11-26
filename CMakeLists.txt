cmake_minimum_required(VERSION 3.10)

project(ReCUm12 VERSION 12.1.100 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(modules/hw)
add_subdirectory(modules/core)
add_subdirectory(modules/gui)
add_subdirectory(modules/rfid)
add_subdirectory(modules/net)
add_subdirectory(modules/utils)

add_subdirectory(apps/recum12_app)
