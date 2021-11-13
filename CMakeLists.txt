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

# unset module path to not get polluted from including projects
unset(CMAKE_MODULE_PATH)

# append custom cmake paths to module search
list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_LIST_DIR}/cmake"
)

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

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
)

include(protocol_generator)
generate_wayland_source_files(${CMAKE_CURRENT_SOURCE_DIR}/cmake/protocols/wlr-screencopy-unstable-v1.xml)
generate_wayland_source_files(${CMAKE_CURRENT_SOURCE_DIR}/cmake/protocols/xdg-output-unstable-v1.xml)

link_wayland_protocols(
    TARGET
        ${PROJECT_NAME}
    PROTOCOLS
        wlr-screencopy-unstable-v1
        xdg-output-unstable-v1
)
