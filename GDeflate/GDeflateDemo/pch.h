/*
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef WIN32
# define NOMINMAX
# include <Windows.h>
# include <directx/d3d12.h>
# include <directx/d3dx12.h>
# include <dxcapi.h>
# include <dxgi1_6.h>
# include <wil/resource.h>
# include <winrt/base.h>
#endif

#include <assert.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
