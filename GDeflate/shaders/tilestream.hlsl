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

static const uint32_t kDefaultTileSize = 64 * 1024;
static const uint32_t kStreamHeaderSize = 8;

struct TileParams
{
    uint32_t inPos;
    uint32_t inSize;
    uint32_t outPos;
    uint32_t outSize;
};

// This should match TileStreamHeader in TileStream.h
//
// uint8_t id;
// uint8_t magic;
// uint16_t numTiles;
// uint32_t reserved0 : 2;
// uint32_t lastTileSize : 18;
// uint32_t reserved1 : 12;

static uint32_t TileStream_GetField(uint32_t value, uint32_t bitsOffset, uint32_t bitsLength)
{
    uint32_t mask = (1u << bitsLength) - 1;
    return (value >> bitsOffset) & mask;
}

struct TileStream
{
    uint32_t m_word1;
    uint32_t m_word2;
    uint32_t m_numTiles;

    static TileStream construct(uint32_t streamInPos)
    {
        TileStream ts;

        ts.m_word1 = input.Load(streamInPos);
        ts.m_word2 = input.Load(streamInPos + 4);
        ts.m_numTiles = TileStream_GetField(ts.m_word1, 16, 16);

        return ts;
    }

    uint32_t GetNumTiles()
    {
        return m_numTiles;
    }

    uint32_t GetLastTileSizeField()
    {
        return TileStream_GetField(m_word2, 2, 18);
    }

    uint32_t GetLastTileSize()
    {
        uint32_t lastTileSize = GetLastTileSizeField();
        return lastTileSize > 0 ? lastTileSize : kDefaultTileSize;
    }

    TileParams GetTileParams(uint32_t streamInPos, uint32_t streamOutPos, uint32_t tileIdx)
    {
        TileParams params;

        const uint32_t tileTablePos = streamInPos + kStreamHeaderSize;

        params.inPos = (tileIdx > 0 ? input.Load(tileTablePos + tileIdx * 4) : 0);

        if (tileIdx == m_numTiles - 1)
            params.inSize = input.Load(tileTablePos);
        else
            params.inSize = input.Load(tileTablePos + (tileIdx + 1) * 4) - params.inPos;

        params.outPos = streamOutPos + tileIdx * kDefaultTileSize;
        params.outSize = tileIdx < m_numTiles - 1 ? kDefaultTileSize : GetLastTileSize();

        const uint32_t streamDataStartPos = tileTablePos + m_numTiles * 4;
        params.inPos += streamDataStartPos;

        return params;
    }
};
