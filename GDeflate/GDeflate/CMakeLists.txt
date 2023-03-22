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

include("../3rdparty/libdeflate.cmake")

set(SOURCES
  GDeflateCompress.cpp
  GDeflateDecompress.cpp
)

set(HEADERS
  config.h
  TileStream.h
  Utils.h
)

set(PUBLIC_HEADERS
  GDeflate.h
)

add_library(GDeflate STATIC ${SOURCES} ${HEADERS} ${PUBLIC_HEADERS})
target_include_directories(GDeflate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(GDeflate PRIVATE cxx_std_17)
target_link_libraries(GDeflate PUBLIC libdeflate_static)
