# Copyright (c) Microsoft Corporation. All rights reserved.

cmake_minimum_required (VERSION 3.19)

project("GDeflateDemo")

set(sources 
    "main.cpp"
    "CompressedFile.h"
    "pch.h"
)

set(libs GDeflate windowsapp.lib dxcompiler.lib)

if (WIN32)
    set(sources ${sources} 
        "GpuDecompressor.cpp"
        "GpuDecompressor.h"
    )

    find_package(directx-dxc CONFIG REQUIRED)
    find_package(directx-headers CONFIG REQUIRED)
    find_package(wil CONFIG REQUIRED)

    set(libs ${libs}     
        Microsoft::DirectXShaderCompiler
        Microsoft::DirectX-Guids Microsoft::DirectX-Headers
        WIL::WIL
        "d3d12.lib" 
        "dxgi.lib"
    )    
else()
    set(libs ${libs} pthread)
endif(WIN32)

add_executable(GDeflateDemo ${sources})

target_compile_features(GDeflateDemo PRIVATE cxx_std_17)
target_precompile_headers(GDeflateDemo PRIVATE pch.h)
target_link_libraries(GDeflateDemo PRIVATE ${libs})

if (WIN32)
    get_target_property(dxil_location Microsoft::DirectXShaderCompiler IMPORTED_LOCATION_RELEASE)
    get_filename_component(dxil_location "${dxil_location}" PATH)


    add_custom_command(TARGET GDeflateDemo 
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/../shaders/gdeflate.hlsl" $<TARGET_FILE_DIR:GDeflateDemo>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/../shaders/tilestream.hlsl" $<TARGET_FILE_DIR:GDeflateDemo>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dxil_location}/dxil.dll" $<TARGET_FILE_DIR:GDeflateDemo>
    )
endif (WIN32)
