cmake_minimum_required(VERSION 3.18)

project("wlroots_screencopy_poc"
    VERSION
        0.1.0
    LANGUAGES
        C CXX
)

if(NOT UNIX)
    message(FATAL_ERROR "Only UNIX systems are supported!")
endif()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt5 COMPONENTS Core REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core Gui REQUIRED)

add_executable(${PROJECT_NAME})
add_subdirectory(src)

target_compile_features(${PROJECT_NAME}
    PUBLIC
        cxx_std_17
)

set_target_properties(${PROJECT_NAME}
    PROPERTIES
        CXX_EXTENSIONS OFF
)

target_link_libraries(${PROJECT_NAME}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
)
