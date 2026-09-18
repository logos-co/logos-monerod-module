cmake_minimum_required(VERSION 3.14)
project(MonerodModulePlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME monerod_module
    SOURCES
        src/monerod_module_impl.h
        src/monerod_module_impl.cpp
        src/loopback_http.h
        src/loopback_http.cpp
    EXTERNAL_LIBS
        monerod_c
    INCLUDE_DIRS
        lib
)

if(NOT APPLE AND NOT WIN32)
    target_link_libraries(monerod_module_module_plugin PRIVATE stdc++ m pthread)
endif()
