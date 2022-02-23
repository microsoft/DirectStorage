//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include <dstorage.h>
#include <string>

class ModelH3D;

void InitializeDStorage();
void ShutdownDStorage();

struct DStorageLoadResult
{
    bool Succeeded;

    // When this fence is set to this value, loading has completed.
    Microsoft::WRL::ComPtr<ID3D12Fence> Fence;
    uint64_t FenceValue;
};

DStorageLoadResult DStorageLoadH3DAInto(ModelH3D* model, const std::wstring& filename);
