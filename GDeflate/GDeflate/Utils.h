/*
 * SPDX-FileCopyrightText: Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corportaion. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>

namespace GDeflate
{
    template<int N, typename T>
    static inline T _align(T a)
    {
        return (a + T(N) - 1) & ~(T(N) - 1);
    }

    template<typename T>
    static inline T _divRoundup(T a, T b)
    {
        return (a + b - 1) / b;
    }

    template<typename T>
    static inline uint32_t _lzCount(T a)
    {
        uint32_t n = 0;

        while (0 == (a & 1) && n < sizeof(T) * 8)
        {
            a >>= 1;
            ++n;
        }

        return n;
    }

    template<typename T>
    static inline T GetBits(uint32_t*& in, uint32_t& offset, uint32_t numBitsToRead)
    {
        constexpr uint32_t kBitsPerBucket = sizeof(*in) * 8;

        T bits = 0;
        uint32_t numBitsConsumed = 0;
        while (numBitsConsumed < numBitsToRead)
        {
            const uint32_t numBits =
                std::min(numBitsToRead - numBitsConsumed, kBitsPerBucket - (offset % kBitsPerBucket));

            const T mask = std::numeric_limits<T>().max() >> (sizeof(T) * 8 - numBits);

            bits |= (T(*in >> (offset % kBitsPerBucket)) & mask) << numBitsConsumed;

            offset += numBits;
            numBitsConsumed += numBits;

            if (0 == offset % kBitsPerBucket)
                in++;
        }

        return bits;
    }
} // namespace GDeflate
