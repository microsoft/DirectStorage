# Copyright (c) Microsoft Corporation. All rights reserved.

cmake_minimum_required (VERSION 3.19)

project("GDeflate")

add_subdirectory("GDeflate")
add_subdirectory("GDeflateDemo")

if (WIN32)
add_subdirectory("GDeflateTest")
endif (WIN32)
