cmake_minimum_required(VERSION 3.21)

project(StereoAligner VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(StereoAligner WIN32
    main.cpp
)

target_link_libraries(StereoAligner PRIVATE Qt6::Widgets)

if(WIN32)
    set_target_properties(StereoAligner PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
