cmake_minimum_required(VERSION 3.20)

# Select the MSVC runtime before project() enables either native language.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(ContentFactory LANGUAGES C CXX)

# This distribution contains Windows x64 executables only; reject unsupported toolchains early.
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "ContentFactory packaging requires Windows and the MSVC toolchain.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ContentFactory packaging requires an x64 build (configure with -A x64).")
endif()
if(CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    message(FATAL_ERROR "ContentFactory packaging requires the x64 generator platform.")
endif()

# Keep all executable artifacts predictable for both single- and multi-configuration generators.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(configuration IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${configuration}" configuration_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${configuration_upper} "${CMAKE_BINARY_DIR}/bin/${configuration}")
endforeach()

# The GACL subtree owns the existing local zstd dependency and enables its static CLI for packaging.
set(CONTENT_FACTORY_BUILD_ZSTD_PROGRAMS ON CACHE BOOL "Build bundled zstd command-line tools" FORCE)
add_subdirectory(GACLContentTool)

# Use the existing GDeflate codec/tool subtree, not its DirectStorage demo or test dependencies.
set(GDEFLATE_BUILD_DEMO OFF CACHE BOOL "Build GDeflate demo" FORCE)
set(GDEFLATE_BUILD_TESTS OFF CACHE BOOL "Build GDeflate tests" FORCE)
set(GDEFLATE_BUILD_CONTENT_TOOL ON CACHE BOOL "Build GDeflateContentTool" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../GDeflate" "${CMAKE_BINARY_DIR}/gdeflate")

# Pin the distributed native executables to the stable layout even if a dependency overrides defaults.
foreach(tool IN ITEMS zstd GDeflateContentTool GACLContentTool)
    if(NOT TARGET ${tool})
        message(FATAL_ERROR "Required ContentFactory native target is missing: ${tool}")
    endif()
    set_target_properties(${tool} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    foreach(configuration IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${configuration}" configuration_upper)
        set_target_properties(${tool} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${configuration_upper} "${CMAKE_BINARY_DIR}/bin/${configuration}")
    endforeach()
endforeach()

# Expose one root-level build target for generators with nested project files.
add_custom_target(ContentFactoryTools DEPENDS zstd GDeflateContentTool GACLContentTool)
