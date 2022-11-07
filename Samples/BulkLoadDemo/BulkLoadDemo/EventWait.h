//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include <wrl/wrappers/corewrappers.h>

#include <cstdlib>
#include <functional>

// Ties together a Win32 event with a Windows Threadpool Wait.
class EventWait
{
    Microsoft::WRL::Wrappers::Event m_event;
    TP_WAIT* m_wait;

public:
    EventWait(void* target, PTP_WAIT_CALLBACK callback)
    {
        m_wait = CreateThreadpoolWait(callback, target, nullptr);
        if (!m_wait)
            std::abort();

        constexpr BOOL manualReset = TRUE;
        constexpr BOOL initialState = FALSE;
        m_event.Attach(CreateEventW(nullptr, manualReset, initialState, nullptr));

        if (!m_event.IsValid())
            std::abort();
    }

    template<typename T, void (T::*FN)()>
    static EventWait Create(T* target)
    {
        auto callback = [](TP_CALLBACK_INSTANCE*, void* context, TP_WAIT*, TP_WAIT_RESULT)
        {
            T* target = reinterpret_cast<T*>(context);
            (target->*FN)();
        };

        return EventWait(target, callback);
    }

    ~EventWait()
    {
        Close();
    }

    void SetThreadpoolWait()
    {
        ResetEvent(m_event.Get());
        ::SetThreadpoolWait(m_wait, m_event.Get(), nullptr);
    }

    bool IsSet() const
    {
        return WaitForSingleObject(m_event.Get(), 0) == WAIT_OBJECT_0;
    }

    void Close()
    {
        if (m_wait)
        {
            WaitForThreadpoolWaitCallbacks(m_wait, TRUE);
            CloseThreadpoolWait(m_wait);
            m_wait = nullptr;
        }
    }

    operator HANDLE()
    {
        return m_event.Get();
    }
};