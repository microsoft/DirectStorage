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

#include "GDeflate.h"
#include "TileStream.h"
#include "Utils.h"
#include "config.h"

#include <assert.h>
#include <libdeflate.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

template<>
struct std::default_delete<libdeflate_gdeflate_compressor>
{
    void operator()(libdeflate_gdeflate_compressor* p) const
    {
        libdeflate_free_gdeflate_compressor(p);
    }
};

namespace GDeflate
{
    static constexpr uint32_t kMaxWorkers = 31;
    static constexpr uint32_t kMinTilesPerWorker = 64;

    struct OutputStreamWrapper
    {
        uint8_t* ptr = nullptr;
        size_t size = 0;
        size_t pos = 0;

        template<typename T>
        void StreamOut(T* d, size_t n = 1)
        {
            const size_t dataSize = sizeof(T) * n;

            if (pos + dataSize > size)
            {
                printf("Fatal: stream overrun!\n");
                return;
            }

            memcpy(ptr + pos, d, dataSize);
            pos += dataSize;
        }

        bool SetPos(size_t n)
        {
            if (n > size)
            {
                printf("Fatal: stream overrun!\n");
                return false;
            }

            pos = n;

            return true;
        }
    };

    struct CompressionContext
    {
        const uint8_t* inputPtr;
        size_t inputSize;

        struct Tile
        {
            std::vector<uint8_t> data;
            size_t uncompressedSize = 0;
        };

        std::vector<Tile> tiles;

        std::atomic_uint32_t globalIndex;
        uint32_t numItems;
    };

    static bool DoCompress(
        uint8_t* output,
        size_t* outputSize,
        const uint8_t* in,
        size_t inSize,
        uint32_t level,
        uint32_t flags)
    {
        if (outputSize == nullptr || output == nullptr || in == nullptr || inSize == 0)
            return false;

        if (inSize > kDefaultTileSize * TileStream::kMaxTiles)
            return false;

        CompressionContext context{};

        context.inputPtr = in;
        context.inputSize = inSize;
        context.numItems = static_cast<uint32_t>((inSize + kDefaultTileSize - 1) / kDefaultTileSize);
        context.tiles.resize(context.numItems);

        auto TileCompressionJob = [&context, level]()
        {
            size_t pageCount = 0;
            const size_t scratchSize = libdeflate_gdeflate_compress_bound(nullptr, kDefaultTileSize, &pageCount);
            assert(pageCount == 1);

            void* scratch = alloca(scratchSize);

            std::unique_ptr<libdeflate_gdeflate_compressor> compressor(libdeflate_alloc_gdeflate_compressor(level));

            while (true)
            {
                const uint32_t tileIndex = context.globalIndex.fetch_add(1, std::memory_order_relaxed);

                if (tileIndex >= context.numItems)
                    break;

                const size_t tilePos = tileIndex * kDefaultTileSize;

                size_t remaining = context.inputSize - tilePos;
                size_t uncompressedSize = std::min<size_t>(remaining, kDefaultTileSize);

                auto& tile = context.tiles[tileIndex];
                tile.uncompressedSize = uncompressedSize;

                libdeflate_gdeflate_out_page compressedPage{scratch, scratchSize};

                libdeflate_gdeflate_compress(
                    compressor.get(),
                    context.inputPtr + tilePos,
                    uncompressedSize,
                    &compressedPage,
                    1);

                tile.data.resize(compressedPage.nbytes);
                memcpy(tile.data.data(), compressedPage.data, compressedPage.nbytes);
            }
        };

        std::thread workers[kMaxWorkers + 1];
        const uint32_t maxWorkers = std::min(kMaxWorkers, std::thread::hardware_concurrency());

        uint32_t numWorkersLeft =
            std::min(maxWorkers, (context.numItems + kMinTilesPerWorker - 1) / kMinTilesPerWorker);

        if (flags & COMPRESS_SINGLE_THREAD)
            numWorkersLeft = 0;

        for (auto& worker : workers)
        {
            if (numWorkersLeft == 0)
                break;

            worker = std::thread([TileCompressionJob]() { TileCompressionJob(); });

            --numWorkersLeft;
        }

        TileCompressionJob();

        for (auto& worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }

        // Compression is done. Prepare the output stream.

        std::vector<uint32_t> tilePtrs;
        size_t dataPos = 0;

        for (auto const& tile : context.tiles)
        {
            tilePtrs.push_back(static_cast<uint32_t>(dataPos));
            dataPos += tile.data.size();
        }

        // tilePtrs[0] is used to store the size of the last tile; all the other
        // elements are offsets to the tile data.
        tilePtrs[0] = static_cast<uint32_t>(context.tiles.back().data.size());

        assert(tilePtrs.size() <= TileStream::kMaxTiles);
        assert(tilePtrs.size() == context.tiles.size());
        assert(tilePtrs.size() == context.numItems);

        OutputStreamWrapper outputStream;
        outputStream.ptr = output;
        outputStream.size = *outputSize;

        // calculate uncompressed size
        size_t uncompressedSize = tilePtrs.size() * kDefaultTileSize;
        size_t tailSize = context.inputSize - (tilePtrs.size() - 1) * kDefaultTileSize;
        if (tailSize < kDefaultTileSize)
            uncompressedSize -= kDefaultTileSize - tailSize;

        assert(uncompressedSize == inSize);

        TileStream header(uncompressedSize);

        assert(tilePtrs.size() == header.numTiles);

        outputStream.StreamOut(&header);
        outputStream.StreamOut(tilePtrs.data(), tilePtrs.size());

        size_t dataOffset = outputStream.pos;

        for (size_t i = 0; i < tilePtrs.size(); ++i)
        {
            CompressionContext::Tile const& tile = context.tiles[i];
            uint32_t tileOffset = (i == 0) ? 0 : tilePtrs[i];

            outputStream.SetPos(dataOffset + tileOffset);
            outputStream.StreamOut(tile.data.data(), tile.data.size());
        }

        *outputSize = outputStream.pos;

        return true;
    }

    size_t CompressBound(size_t size)
    {
        size_t numTiles = std::min(size_t(TileStream::kMaxTiles), (size + kDefaultTileSize - 1) / kDefaultTileSize);
        numTiles = std::max(size_t(1), numTiles);

        const size_t tileSize = kDefaultTileSize +
                                // Tile header. Ideally need to make it a part of compressor API.
                                (sizeof(uint32_t) + 4 * 208 + 4 * 8);

        return numTiles * tileSize + sizeof(TileStream) + sizeof(uint64_t);
    }

    bool Compress(uint8_t* output, size_t* outputSize, const uint8_t* in, size_t inSize, uint32_t level, uint32_t flags)
    {
        return DoCompress(output, outputSize, in, inSize, level, flags);
    }

} // namespace GDeflate
