cmake_minimum_required(VERSION 3.10)

project(ReCUm12 VERSION 12.1.100 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-3.0)

message(STATUS "GTKMM include dirs: ${GTKMM_INCLUDE_DIRS}")
message(STATUS "GTKMM libraries   : ${GTKMM_LIBRARIES}")
message(STATUS "GTKMM cflags      : ${GTKMM_CFLAGS_OTHER}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(modules/hw)
add_subdirectory(modules/core)
add_subdirectory(modules/gui)
add_subdirectory(modules/rfid)
add_subdirectory(modules/net)
add_subdirectory(modules/utils)

add_subdirectory(apps/recum12_app)
