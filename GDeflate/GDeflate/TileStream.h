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

#pragma once

#include "Utils.h"
#include "config.h"

#include <assert.h>
#include <stdint.h>

#include <string>

namespace GDeflate
{
#pragma pack(push, 1)
    struct TileStream
    {
        static constexpr uint32_t kMaxTiles = (1 << 16) - 1;

        uint8_t id;
        uint8_t magic;

        uint16_t numTiles;

        uint32_t tileSizeIdx : 2; // this must be set to 1
        uint32_t lastTileSize : 18;
        uint32_t reserved1 : 12;

        TileStream(size_t uncompressedSize)
        {
            memset(this, 0, sizeof(*this));
            tileSizeIdx = 1;
            SetCodecId(kGDeflateId);
            SetUncompressedSize(uncompressedSize);
        }

        bool IsValid() const
        {
            return id == (magic ^ 0xff);
        }

        size_t GetUncompressedSize() const
        {
            return numTiles * kDefaultTileSize - (lastTileSize == 0 ? 0 : kDefaultTileSize - lastTileSize);
        }

    private:
        void SetCodecId(uint8_t inId)
        {
            id = inId;
            magic = inId ^ 0xff;
        }

        void SetUncompressedSize(size_t size)
        {
            numTiles = static_cast<uint16_t>(size / kDefaultTileSize);
            lastTileSize = static_cast<uint32_t>(size - numTiles * kDefaultTileSize);

            numTiles += lastTileSize != 0 ? 1 : 0;
        }
    };

#pragma pack(pop)

    static_assert(sizeof(TileStream) == 8, "Tile stream header size overrun!");

} // namespace GDeflate
