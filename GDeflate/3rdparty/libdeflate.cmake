#
# SPDX-FileCopyrightText: Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set(LIBDEFLATE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libdeflate")

set(SOURCES
  ${LIBDEFLATE_DIR}/lib/adler32.c
  ${LIBDEFLATE_DIR}/lib/crc32.c
  ${LIBDEFLATE_DIR}/lib/deflate_compress.c
  ${LIBDEFLATE_DIR}/lib/deflate_decompress.c
  ${LIBDEFLATE_DIR}/lib/gdeflate_compress.c
  ${LIBDEFLATE_DIR}/lib/gdeflate_decompress.c
  ${LIBDEFLATE_DIR}/lib/gzip_compress.c
  ${LIBDEFLATE_DIR}/lib/gzip_decompress.c
  ${LIBDEFLATE_DIR}/lib/utils.c
  ${LIBDEFLATE_DIR}/lib/zlib_compress.c
  ${LIBDEFLATE_DIR}/lib/zlib_decompress.c
  ${LIBDEFLATE_DIR}/lib/x86/cpu_features.c
)

set(HEADERS
  ${LIBDEFLATE_DIR}/lib/adler32_vec_template.h
  ${LIBDEFLATE_DIR}/lib/bt_matchfinder.h
  ${LIBDEFLATE_DIR}/lib/crc32_table.h
  ${LIBDEFLATE_DIR}/lib/crc32_vec_template.h
  ${LIBDEFLATE_DIR}/lib/decompress_template.h
  ${LIBDEFLATE_DIR}/lib/gdeflate_decompress_template.h
  ${LIBDEFLATE_DIR}/lib/deflate_compress.h
  ${LIBDEFLATE_DIR}/lib/deflate_constants.h
  ${LIBDEFLATE_DIR}/lib/gzip_constants.h
  ${LIBDEFLATE_DIR}/lib/hc_matchfinder.h
  ${LIBDEFLATE_DIR}/lib/lib_common.h
  ${LIBDEFLATE_DIR}/lib/matchfinder_common.h
  ${LIBDEFLATE_DIR}/lib/unaligned.h
  ${LIBDEFLATE_DIR}/lib/zlib_constants.h
  ${LIBDEFLATE_DIR}/lib/x86/adler32_impl.h
  ${LIBDEFLATE_DIR}/lib/x86/cpu_features.h
  ${LIBDEFLATE_DIR}/lib/x86/crc32_impl.h
  ${LIBDEFLATE_DIR}/lib/x86/crc32_pclmul_template.h
  ${LIBDEFLATE_DIR}/lib/x86/decompress_impl.h
  ${LIBDEFLATE_DIR}/lib/x86/matchfinder_impl.h
)

set(PUBLIC_HEADERS
  ${LIBDEFLATE_DIR}/libdeflate.h
)

if (WIN32)
# The follow warnings have been disabled to be able to compile this library without making modifications
# to the original source.
#  C4244 'return': conversion from 'uint64_t' to 'unsigned int', possible loss of data - compiler_msc.h 68
#  C4127 conditional expression is constant - common_defs.h 290 
#  C4267 'function': conversion from 'size_t' to 'uint32_t', possible loss of data - common_defs.h  291 
#  C4100 'c': unreferenced formal parameter - deflate_compress.c    2428    
#  C4245 '=': conversion from 'int' to 'unsigned int', signed/unsigned mismatch  - deflate_decompress.c 752 
#  C4456 declaration of 'len' hides previous local declaration - deflate_compress.c 1118    
#  C4018 '>=': signed/unsigned mismatch - decompress_template.h 297 
#  C4146 unary minus operator applied to unsigned type, result still unsigned - bt_matchfinder.h    219 
#  C4310 cast truncates constant value - bt_matchfinder.h   219 
add_compile_options(/wd4244 /wd4127 /wd4267 /wd4100 /wd4245 /wd4456 /wd4018 /wd4146 /wd4310)
endif (WIN32)

include_directories(${LIBDEFLATE_DIR})
add_library(libdeflate_static STATIC ${SOURCES} ${HEADERS} ${PUBLIC_HEADERS})
target_include_directories(libdeflate_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(libdeflate_static PROPERTIES
  OUTPUT_NAME deflate
  PUBLIC_HEADER libdeflate.h)