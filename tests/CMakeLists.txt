cmake_minimum_required(VERSION 3.14)
project(MonerodModuleTests LANGUAGES CXX)

include(LogosTest)

# Settings and argv only: monerod_settings.cpp needs neither the node nor IPC.
logos_test(
    NAME monerod_module_tests
    MODULE_SOURCES
        ../src/monerod_settings.cpp
    TEST_SOURCES
        main.cpp
        settings_test.cpp
    EXTRA_INCLUDES
        ../src
)
