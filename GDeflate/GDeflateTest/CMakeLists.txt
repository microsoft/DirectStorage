# Copyright (c) Microsoft Corporation. All rights reserved.

cmake_minimum_required (VERSION 3.19)

project("GDeflateTest")

add_executable(GDeflateTest
"GDeflateTest.cpp"
)

target_compile_features(GDeflateTest PRIVATE cxx_std_17)

find_program(NUGET_EXE NAMES nuget)
exec_program(${NUGET_EXE}
    ARGS install -Source https://api.nuget.org/v3/index.json "Microsoft.Direct3D.DirectStorage" -Version 1.2.0 -ExcludeVersion -OutputDirectory ${CMAKE_BINARY_DIR}/packages)


include_directories(${CMAKE_BINARY_DIR}/packages/Microsoft.Direct3D.DirectStorage/native/include)
target_link_directories(GDeflateTest PRIVATE ${CMAKE_BINARY_DIR}/packages/Microsoft.Direct3D.DirectStorage/native/lib/$ENV{VSCMD_ARG_TGT_ARCH})
target_link_libraries(GDeflateTest PRIVATE dstorage.lib)

target_link_libraries(GDeflateTest PRIVATE GDeflate)

add_custom_command(TARGET GDeflateTest
COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/packages/Microsoft.Direct3D.DirectStorage/native/bin/$ENV{VSCMD_ARG_TGT_ARCH}/DStorage.dll" $<TARGET_FILE_DIR:GDeflateTest>
COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/packages/Microsoft.Direct3D.DirectStorage/native/bin/$ENV{VSCMD_ARG_TGT_ARCH}/DStorageCore.dll" $<TARGET_FILE_DIR:GDeflateTest>
)
