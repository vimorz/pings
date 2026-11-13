cmake_minimum_required(VERSION 3.24)
project(pings VERSION 0.7.0 LANGUAGES CXX)

include(CTest)

option(PINGS_BUILD_CONFIG "Build YAML configuration support" ON)
option(PINGS_BUILD_APPS "Build example applications" ON)

if(PINGS_BUILD_APPS AND NOT PINGS_BUILD_CONFIG)
    message(FATAL_ERROR
        "Current applications require PINGS_BUILD_CONFIG=ON. "
        "Set PINGS_BUILD_APPS=OFF to build without configuration."
    )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

cmake_policy(SET CMP0111 NEW)

find_package(fmt CONFIG REQUIRED)
if(PINGS_BUILD_APPS)
    find_package(ftxui CONFIG REQUIRED)
endif()
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)
if(PINGS_BUILD_CONFIG)
    find_package(yaml-cpp CONFIG REQUIRED)
endif()
message(STATUS "Boost_INCLUDE_DIRS = ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost_LIBRARIES = ${Boost_LIBRARIES}")

add_library(pings_core
    src/async_pinger.cpp

    src/ping_handle.cpp
    src/ping_options.cpp

    src/batch_handle.cpp
    src/detail/batch_state.cpp

    src/repeat_handle.cpp
    src/repeat_options.cpp

    src/detail/icmp_codec.cpp
    src/detail/icmpv6_codec.cpp
    src/detail/ipv4_parser.cpp
)

add_library(pings::core ALIAS pings_core)

target_include_directories(pings_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include

    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/detail
        ${PROJECT_SOURCE_DIR}/src/utils
)

target_link_libraries(pings_core
  PUBLIC
    Boost::headers
    Boost::system

  PRIVATE
    fmt::fmt
    Threads::Threads
)

if(PINGS_BUILD_CONFIG)
    add_library(pings_config
        src/config/config_mapping.cpp
        src/config/config_validation.cpp
        src/config/config_loader.cpp
    )

    add_library(pings::config ALIAS pings_config)

    target_link_libraries(pings_config
        PUBLIC
            pings::core
        PRIVATE
            yaml-cpp::yaml-cpp
    )
endif()


if(PINGS_BUILD_APPS)
    # Application
    add_executable(pings
        app/main.cpp
        examples/batch_example.cpp
        examples/config_example.cpp
        examples/repeat_example.cpp
        examples/single_example.cpp
    )
    # Application
    add_executable(pings_tui
        app/tui_main.cpp
        app/pings_ui.cpp
    )
    # add_executable(pings_tui
        # app/main.cpp
        # app/pings_ui.cpp
        # examples/batch_example.cpp
        # examples/config_example.cpp
        # examples/repeat_example.cpp
        # examples/single_example.cpp
    # )

    target_include_directories(pings
    PRIVATE
        ${PROJECT_SOURCE_DIR}/examples
        ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/include/utils
    )

    # target_include_directories(pings_tui
    # PRIVATE
    #     ${PROJECT_SOURCE_DIR}/examples
    #     ${PROJECT_SOURCE_DIR}/tests
    #     ${PROJECT_SOURCE_DIR}/include/utils
    # )

    target_link_libraries(pings
    PRIVATE
        pings::core
        pings::config
        fmt::fmt
    )

    target_link_libraries(pings_tui
    PRIVATE
        # pings::core
        # pings::config
        # fmt::fmt
        ftxui::component
        ftxui::dom
        ftxui::screen
    )
endif()

# Compiler warnings
foreach(pings_target IN ITEMS pings_core pings_config pings pings_tui)
    if(TARGET ${pings_target})
        if(MSVC)
            target_compile_options(${pings_target} PRIVATE
                /W4
                /EHsc
                /utf-8
                /Zc:preprocessor
            )
        else()
            target_compile_options(${pings_target} PRIVATE
                -Wall
                -Wextra
                -Wpedantic
            )
        endif()
    endif()
endforeach()


# Windows
if(WIN32)
    target_compile_definitions(pings_core
      PUBLIC
        _WIN32_WINNT=0x0A00
      PRIVATE
        WIN32_LEAN_AND_MEAN
    )
    if(PINGS_BUILD_APPS)
        target_compile_definitions(pings
        PRIVATE
            WIN32_LEAN_AND_MEAN
        )
    endif()
endif()

# Debug
target_compile_definitions(pings_core
  PRIVATE
    $<$<CONFIG:Debug>:DEBUG_ENABLED>
)

if(PINGS_BUILD_APPS)
    target_compile_definitions(pings
    PRIVATE
        $<$<CONFIG:Debug>:DEBUG_ENABLED>
    )

    # target_compile_definitions(pings_tui
    # PRIVATE ENABLE_UI)

    add_custom_command(
        TARGET pings
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/app/config.yml
            $<TARGET_FILE_DIR:pings>/config.yml
    )
endif()

if(BUILD_TESTING)
    function(pings_add_test target_name test_name source_file)
        add_executable(${target_name}
            "${source_file}"
        )

        target_link_libraries(${target_name}
            PRIVATE pings::core
        )

        if(MSVC)
            target_compile_options(${target_name} PRIVATE
                /W4
                /EHsc
                /utf-8
                /Zc:preprocessor
            )
        else()
            target_compile_options(${target_name} PRIVATE
                -Wall
                -Wextra
                -Wpedantic
            )
        endif()

        add_test(
            NAME ${test_name}
            COMMAND ${target_name}
        )
    endfunction()

    pings_add_test(
        ipv4_parser_test
        ipv4_parser
        tests/ipv4_parser_test.cpp
    )

    pings_add_test(
        pings_options_test
        ping_options
        tests/ping_options_test.cpp
    )

    pings_add_test(
        pings_checksum_test
        icmp_checksum
        tests/icmp_checksum_test.cpp
    )

    pings_add_test(
        pings_echo_reply_test
        icmp_echo_reply
        tests/icmp_echo_reply_test.cpp
    )

    pings_add_test(
        pings_icmpv6_codec_test
        icmpv6_codec
        tests/icmpv6_codec_test.cpp
    )

    if(PINGS_BUILD_CONFIG)
        pings_add_test(
            pings_config_mapping_test
            config_mapping
            tests/config_mapping_test.cpp
        )

        pings_add_test(
            pings_config_loader_test
            config_loader
            tests/config_loader_test.cpp
        )

        target_link_libraries(pings_config_mapping_test
            PRIVATE pings::config
        )

        target_link_libraries(pings_config_loader_test
            PRIVATE pings::config
        )

        set_tests_properties(config_loader PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/tests/data/config"
        )
    endif()

    pings_add_test(
        pings_repeat_statistics_test
        repeat_statistics
        tests/repeat_statistics_test.cpp
    )

    pings_add_test(
        pings_lifecycle_test
        pinger_lifecycle
        tests/pinger_lifecycle_test.cpp
    )

    set_tests_properties(pinger_lifecycle PROPERTIES
        TIMEOUT 10
    )

    pings_add_test(
        pings_batch_api_test
        batch_api
        tests/batch_api_test.cpp
    )

    set_tests_properties(batch_api PROPERTIES
        TIMEOUT 10
    )

    pings_add_test(
        pings_repeat_api_test
        repeat_api
        tests/repeat_api_test.cpp
    )

    set_tests_properties(repeat_api PROPERTIES
        TIMEOUT 10
    )

    pings_add_test(
        pings_batch_state_test
        batch_state
        tests/batch_state_test.cpp
    )

    target_include_directories(pings_batch_state_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/detail"
    )

    pings_add_test(
        pings_reply_deadline_test
        reply_deadline
        tests/reply_deadline_test.cpp
    )

    target_include_directories(pings_reply_deadline_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/detail"
    )

    set_tests_properties(batch_state PROPERTIES
        TIMEOUT 10
    )

    target_include_directories(pings_repeat_statistics_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/detail"
    )

    target_include_directories(ipv4_parser_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/detail"
    )

    target_include_directories(pings_checksum_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/detail"
    )

    target_include_directories(pings_echo_reply_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/detail"
    )

    target_include_directories(pings_icmpv6_codec_test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/detail"
    )

endif()