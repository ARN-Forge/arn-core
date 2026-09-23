cmake_minimum_required(VERSION 3.20)

if (POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

project(arn_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Dependencies
find_package(Threads REQUIRED)

if (NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.11.3 QUIET)
    if (NOT nlohmann_json_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.11.3
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(nlohmann_json)
        if (NOT nlohmann_json_POPULATED)
            FetchContent_Populate(nlohmann_json)
        endif()
        add_library(nlohmann_json INTERFACE IMPORTED GLOBAL)
        target_include_directories(nlohmann_json INTERFACE
            $<BUILD_INTERFACE:${nlohmann_json_SOURCE_DIR}/include>
        )
        add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    endif()
endif()

if (NOT TARGET httplib::httplib)
    find_package(httplib 0.18.3 QUIET)
    if (NOT httplib_FOUND)
        find_package(OpenSSL REQUIRED)
        include(FetchContent)
        FetchContent_Declare(
            cpp_httplib
            GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
            GIT_TAG v0.18.3
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(cpp_httplib)
        if (NOT cpp_httplib_POPULATED)
            FetchContent_Populate(cpp_httplib)
        endif()
        add_library(httplib INTERFACE IMPORTED GLOBAL)
        target_include_directories(httplib INTERFACE
            $<BUILD_INTERFACE:${cpp_httplib_SOURCE_DIR}>
        )
        target_compile_definitions(httplib INTERFACE
            CPPHTTPLIB_OPENSSL_SUPPORT
        )
        target_link_libraries(httplib INTERFACE
            Threads::Threads
            OpenSSL::SSL
            OpenSSL::Crypto
            $<$<PLATFORM_ID:Windows>:ws2_32>
            $<$<PLATFORM_ID:Windows>:crypt32>
        )
        add_library(httplib::httplib ALIAS httplib)
    endif()
endif()

add_library(arn_core
    src/version.cpp
    src/net/http_client.cpp
    src/net/model_parser.cpp
    src/tool/tool_registry.cpp
    src/providers/provider_utils.cpp
    src/providers/model_provider.cpp
    src/providers/gemini_provider.cpp
    src/providers/deepseek_provider.cpp
    src/providers/openrouter_provider.cpp
    src/providers/provider_factory.cpp
    src/agent/agent_session.cpp
)

set_target_properties(arn_core PROPERTIES EXPORT_NAME core)
add_library(arn::core ALIAS arn_core)
add_library(arn::arn_core ALIAS arn_core)

target_include_directories(arn_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        src
)

target_compile_features(arn_core PUBLIC cxx_std_23)

target_link_libraries(arn_core
    PUBLIC
        $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
        $<INSTALL_INTERFACE:nlohmann_json::nlohmann_json>
        $<BUILD_INTERFACE:httplib::httplib>
        $<INSTALL_INTERFACE:httplib::httplib>
        Threads::Threads
)

if (MSVC)
    target_compile_options(arn_core PRIVATE /utf-8)
endif()

# Installation and packaging
install(TARGETS arn_core
    EXPORT arn_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT arn_coreTargets
    FILE arn_coreTargets.cmake
    NAMESPACE arn::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/arn_core
)

set(ARN_CORE_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/arn_core")

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/arn_coreConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/arn_coreConfig.cmake
    INSTALL_DESTINATION ${ARN_CORE_CONFIG_INSTALL_DIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/arn_coreConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/arn_coreConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/arn_coreConfigVersion.cmake
    DESTINATION ${ARN_CORE_CONFIG_INSTALL_DIR}
)

# Tests
if (BUILD_TESTING)
    add_executable(arn_core_sse_test tests/sse_decoder_test.cpp)
    target_link_libraries(arn_core_sse_test PRIVATE arn::core)
    add_test(NAME arn_core_sse COMMAND arn_core_sse_test)

    add_executable(arn_core_retry_test tests/http_retry_test.cpp)
    target_link_libraries(arn_core_retry_test PRIVATE arn::core)
    add_test(NAME arn_core_retry COMMAND arn_core_retry_test)

    add_executable(arn_core_tool_registry_test tests/tool_registry_test.cpp)
    target_link_libraries(arn_core_tool_registry_test PRIVATE arn::core)
    add_test(NAME arn_core_tool_registry COMMAND arn_core_tool_registry_test)

    add_executable(arn_core_provider_test tests/provider_test.cpp)
    target_include_directories(arn_core_provider_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(arn_core_provider_test PRIVATE arn::core)
    add_test(NAME arn_core_provider COMMAND arn_core_provider_test)

    add_executable(arn_core_agent_session_test tests/agent_session_test.cpp)
    target_include_directories(arn_core_agent_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(arn_core_agent_session_test PRIVATE arn::core)
    add_test(NAME arn_core_agent_session COMMAND arn_core_agent_session_test)

    add_executable(arn_core_openrouter_test tests/openrouter_test.cpp)
    target_include_directories(arn_core_openrouter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(arn_core_openrouter_test PRIVATE arn::core)
    add_test(NAME arn_core_openrouter COMMAND arn_core_openrouter_test)

    if (MSVC)
        target_compile_options(arn_core_sse_test PRIVATE /utf-8)
        target_compile_options(arn_core_retry_test PRIVATE /utf-8)
        target_compile_options(arn_core_tool_registry_test PRIVATE /utf-8)
        target_compile_options(arn_core_provider_test PRIVATE /utf-8)
        target_compile_options(arn_core_agent_session_test PRIVATE /utf-8)
        target_compile_options(arn_core_openrouter_test PRIVATE /utf-8)
    endif()
endif()
