cmake_minimum_required(VERSION 3.14)
project(MonerodModuleTests LANGUAGES CXX)

include(LogosTest)

# Settings, argv and following Tor's port: neither needs the node nor IPC.
logos_test(
    NAME monerod_module_tests
    MODULE_SOURCES
        ../src/monerod_settings.cpp
        ../src/tor_follow.cpp
    TEST_SOURCES
        main.cpp
        settings_test.cpp
        tor_follow_test.cpp
    EXTRA_INCLUDES
        ../src
)
