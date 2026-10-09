# Copyright (c) Microsoft Corporation. All rights reserved.

cmake_minimum_required (VERSION 3.19)

project("GDeflate")

option(GDEFLATE_BUILD_DEMO "Build the DirectStorage GDeflate demo" ON)
option(GDEFLATE_BUILD_TESTS "Build the GDeflate test executable" ON)
option(GDEFLATE_BUILD_CONTENT_TOOL "Build the batch content processor" ON)

add_subdirectory("GDeflate")

if (GDEFLATE_BUILD_DEMO)
add_subdirectory("GDeflateDemo")
endif ()

if (WIN32 AND GDEFLATE_BUILD_TESTS)
add_subdirectory("GDeflateTest")
endif ()

if (GDEFLATE_BUILD_CONTENT_TOOL)
add_subdirectory("GDeflateContentTool")
endif ()
