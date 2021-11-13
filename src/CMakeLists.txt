target_sources(${PROJECT_NAME}
    PRIVATE
        main.cpp
        wayland/wlroots_screen_capture.hpp
        wayland/wlroots_screen_capture.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)
