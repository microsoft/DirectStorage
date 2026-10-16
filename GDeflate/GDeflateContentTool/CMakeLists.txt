# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

add_executable(GDeflateContentTool main.cpp)
target_compile_features(GDeflateContentTool PRIVATE cxx_std_17)
target_link_libraries(GDeflateContentTool PRIVATE GDeflate)
