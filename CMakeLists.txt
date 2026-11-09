cmake_minimum_required(VERSION 3.21)

project(StereoAligner VERSION 1.1.2 LANGUAGES CXX)

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

    # opencv_world links these image-codec and serialization libraries
    # dynamically in the vcpkg MinGW build.  They are not discovered by
    # windeployqt, so copy them beside the executable for portable releases.
    set(OpenCV_TRANSITIVE_RUNTIME_DLLS
        libabseil_dll.dll
        libjpeg-62.dll
        liblzma.dll
        libpng16.dll
        libprotobuf.dll
        libsharpyuv.dll
        libtiff.dll
        libwebp.dll
        libwebpdecoder.dll
        libwebpdemux.dll
        libwebpmux.dll
    )
    foreach(runtime_dll IN LISTS OpenCV_TRANSITIVE_RUNTIME_DLLS)
        if(EXISTS "${OpenCV_PREFIX}/bin/${runtime_dll}")
            add_custom_command(TARGET StereoAligner POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${OpenCV_PREFIX}/bin/${runtime_dll}"
                    "$<TARGET_FILE_DIR:StereoAligner>"
            )
        endif()
    endforeach()
endif()

if(WIN32)
    set_target_properties(StereoAligner PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
