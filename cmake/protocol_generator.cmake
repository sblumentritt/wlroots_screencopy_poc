include_guard(GLOBAL)

function(generate_wayland_source_files protocol_path)
    # search for required program
    if(NOT DEFINED wayland_scanner_executable)
        find_program(wayland_scanner_executable NAMES "wayland-scanner")

        mark_as_advanced(FORCE
            wayland_scanner_executable
        )

        if(NOT wayland_scanner_executable)
            message(WARNING "wayland-scanner was not found!")
            return()
        else()
            message(STATUS "Found wayland-scanner: ${wayland_scanner_executable}")
        endif()
    endif()

    get_filename_component(protocol_name_with_extension ${protocol_path} NAME)
    string(REGEX REPLACE "\\.[^.]*$" "" protocol_name ${protocol_name_with_extension})

    # generate public header files
    execute_process(
        COMMAND
            ${wayland_scanner_executable} client-header
            ${protocol_path}
            ${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-client-protocol.h
        WORKING_DIRECTORY
            ${PROJECT_SOURCE_DIR}
        OUTPUT_QUIET
        ERROR_QUIET
    )

    # generate private source files which will be compiles later as static libs
    execute_process(
        COMMAND
            ${wayland_scanner_executable} private-code
            ${protocol_path}
            ${CMAKE_CURRENT_BINARY_DIR}/${protocol_name}-protocol.c
        WORKING_DIRECTORY
            ${PROJECT_SOURCE_DIR}
        OUTPUT_QUIET
        ERROR_QUIET
    )
endfunction()

function(link_wayland_protocols)
    # define arguments for cmake_parse_arguments
    list(APPEND one_value_args
        TARGET
    )
    list(APPEND multi_value_args
        PROTOCOLS
    )

    # use cmake helper function to parse passed arguments
    cmake_parse_arguments(
        tpre
        ""
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    # check for required arguments
    if(NOT DEFINED tpre_TARGET)
        message(FATAL_ERROR "TARGET argument required!")
    endif()

    if(NOT DEFINED tpre_PROTOCOLS)
        message(FATAL_ERROR "PROTOCOLS argument required!")
    endif()

    set(wayland_protocols_target "wayland_protocols")

    if(NOT TARGET ${wayland_protocols_target})
        # TODO: should check for error when not found
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(wayland_client wayland-client>=1.13.0 REQUIRED)

        set(protocol_headers "")
        set(protocol_sources "")
        foreach(name IN ITEMS ${tpre_PROTOCOLS})
            list(APPEND protocol_headers ${CMAKE_CURRENT_BINARY_DIR}/${name}-client-protocol.h)
            list(APPEND protocol_sources ${CMAKE_CURRENT_BINARY_DIR}/${name}-protocol.c)
        endforeach()

        add_library(${wayland_protocols_target} STATIC ${protocol_headers} ${protocol_sources})

        target_include_directories(${wayland_protocols_target} PUBLIC
            ${CMAKE_CURRENT_BINARY_DIR}
            ${WAYLAND_CLIENT_INCLUDE_DIRS}
        )

        target_link_libraries(${wayland_protocols_target} PUBLIC
            ${WAYLAND_CLIENT_LIBRARIES}
        )
    endif()

    target_link_libraries(${tpre_TARGET} PRIVATE
        ${wayland_protocols_target}
    )
endfunction()
