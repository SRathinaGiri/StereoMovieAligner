cmake_minimum_required(VERSION 3.21)

project(StereoAligner VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(OpenCV REQUIRED COMPONENTS core imgproc features2d calib3d)

qt_add_executable(StereoAligner WIN32
    main.cpp
)

target_include_directories(StereoAligner PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(StereoAligner PRIVATE Qt6::Widgets ${OpenCV_LIBS})

if(WIN32 AND TARGET opencv_core)
    get_filename_component(OpenCV_PREFIX "${OpenCV_DIR}/../.." ABSOLUTE)
    add_custom_command(TARGET StereoAligner POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:opencv_core>"
            "$<TARGET_FILE_DIR:StereoAligner>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OpenCV_PREFIX}/bin/libz.dll"
            "$<TARGET_FILE_DIR:StereoAligner>"
        COMMENT "Copying OpenCV runtime dependencies"
    )
endif()

if(WIN32)
    set_target_properties(StereoAligner PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
