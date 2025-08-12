cmake_minimum_required(VERSION 3.21)

# C++ settings
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Handle Apple arch
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

# Project info
project(AutomaticMacroMaker VERSION 1.0.0)

# Gather source files (recursively in src/)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

# Build mod as shared library
add_library(${PROJECT_NAME} SHARED ${SOURCES})

# Find Geode SDK
if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

# Add Geode SDK
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Set up the Geode mod
setup_geode_mod(${PROJECT_NAME}
    AUTHOR "entity12208"
    DESCRIPTION "Automatically generates a frame-perfect .gdr macro for any level with export UI."
    VERSION ${PROJECT_VERSION}
    RESOURCES resources
)
