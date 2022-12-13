/*
 * SPDX-FileCopyrightText: Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
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

#include "TileStream.h"
#include "Utils.h"

#include <libdeflate.h>

#include <algorithm>
#include <atomic>
#include <thread>

template<>
struct std::default_delete<libdeflate_gdeflate_decompressor>
{
    void operator()(libdeflate_gdeflate_decompressor* p) const
    {
        libdeflate_free_gdeflate_decompressor(p);
    }
};

namespace GDeflate
{
    static constexpr uint32_t kMaxWorkers = 31;

    static bool ValidateStream(const TileStream* header)
    {
        if (!header->IsValid())
        {
            printf("Malformed stream encountered.\n");
            return false;
        }

        if (header->id != kGDeflateId)
        {
            printf("Unknown stream format: %d\n", header->id);
            return false;
        }

        return true;
    }

    struct DecompressionContext
    {
        const uint8_t* inputPtr;
        size_t inputSize;

        uint8_t* outputPtr;
        size_t outputSize;

        std::atomic_uint32_t globalIndex;
        uint32_t numItems;
    };

    static void TileDecompressionJob(DecompressionContext& context, uint32_t compressorId)
    {
        std::unique_ptr<libdeflate_gdeflate_decompressor> decompressor(libdeflate_alloc_gdeflate_decompressor());

        const uint32_t* tileOffsets = reinterpret_cast<const uint32_t*>(context.inputPtr + sizeof(TileStream));
        const uint8_t* inDataPtr = reinterpret_cast<const uint8_t*>(tileOffsets + context.numItems);

        while (true)
        {
            const uint32_t tileIndex = context.globalIndex.fetch_add(1, std::memory_order_relaxed);

            if (tileIndex >= context.numItems)
                break;

            const size_t tileOffset = tileIndex > 0 ? tileOffsets[tileIndex] : 0;
            libdeflate_gdeflate_in_page compressedPage{};
            compressedPage.data = inDataPtr + tileOffset;
            compressedPage.nbytes =
                tileIndex < context.numItems - 1 ? tileOffsets[tileIndex + 1] - tileOffset : tileOffsets[0];

            auto outputOffset = tileIndex * kDefaultTileSize;

            libdeflate_gdeflate_decompress(
                decompressor.get(),
                &compressedPage,
                1,
                context.outputPtr + outputOffset,
                static_cast<size_t>(kDefaultTileSize),
                nullptr);
        }
    }

    bool Decompress(uint8_t* output, size_t outputSize, const uint8_t* in, size_t inSize, uint32_t numWorkers)
    {
        if (nullptr == output || nullptr == in || 0 == outputSize || 0 == inSize)
            return false;

        numWorkers = std::min(kMaxWorkers, numWorkers);
        numWorkers = std::max(1u, numWorkers);

        auto header = reinterpret_cast<const TileStream*>(in);

        if (!ValidateStream(header))
            return false;

        std::thread workers[kMaxWorkers];

        // Run a tile per thread
        header = reinterpret_cast<const TileStream*>(in);

        if (!ValidateStream(header))
        {
            return false;
        }

        DecompressionContext context{};

        context.inputPtr = in;
        context.inputSize = inSize;

        context.outputPtr = output;
        context.outputSize = header->GetUncompressedSize();

        context.globalIndex = 0;
        context.numItems = header->numTiles;

        uint32_t numWorkersLeft = context.numItems > (2 * numWorkers) ? numWorkers : 1;
        const uint32_t compressorId = header->id;

        for (auto& worker : workers)
        {
            if (numWorkersLeft == 1)
                break;

            worker = std::thread([&context, compressorId]() { TileDecompressionJob(context, compressorId); });

            --numWorkersLeft;
        }

        TileDecompressionJob(context, compressorId);

        for (auto& worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }

        return true;
    }
} // namespace GDeflate
