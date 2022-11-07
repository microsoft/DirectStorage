//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include <memory>

//
// This corresponds to a marc::Region that owns the memory it is loaded to.
//
template<typename T>
class MemoryRegion
{
    std::unique_ptr<char[]> m_buffer;

public:
    MemoryRegion() = default;

    MemoryRegion(std::unique_ptr<char[]> buffer)
        : m_buffer(std::move(buffer))
    {
    }

    char* Data()
    {
        return m_buffer.get();
    }

    T* Get()
    {
        return reinterpret_cast<T*>(m_buffer.get());
    }

    T* operator->()
    {
        return reinterpret_cast<T*>(m_buffer.get());
    }

    T const* operator->() const
    {
        return reinterpret_cast<T const*>(m_buffer.get());
    }
};
