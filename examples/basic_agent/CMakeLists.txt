cmake_minimum_required(VERSION 3.20)
project(basic_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# When built standalone or from installed package
if (NOT TARGET arn::core)
    find_package(arn_core 0.1.0 CONFIG REQUIRED)
endif()

add_executable(basic_agent main.cpp)
target_link_libraries(basic_agent PRIVATE arn::core)

if (MSVC)
    target_compile_options(basic_agent PRIVATE /utf-8)
endif()
