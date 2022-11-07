//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
#define _WIN32_DCOM

#include "CpuPerformance.h"

#include <Utility.h>
#include <wbemidl.h>
#include <wil/Resource.h>
#include <winrt/base.h>

#include <optional>

#pragma comment(lib, "wbemuuid.lib")

using winrt::check_hresult;
using winrt::com_ptr;

class CpuPerformanceMonitor
{
    com_ptr<IWbemLocator> m_locator;
    com_ptr<IWbemServices> m_services;
    com_ptr<IWbemRefresher> m_refresher;
    com_ptr<IWbemClassObject> m_perfData;

    std::thread m_workerThread;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    uint64_t m_lastPercentProcessorTime = 0;
    uint64_t m_lastTimestampPerfTime = 0;

    float m_maxPercentProcessorTime;

    bool m_quit = false;

public:
    CpuPerformanceMonitor()
    {
        // Adapted from https://learn.microsoft.com/en-us/windows/win32/wmisdk/initializing-com-for-a-wmi-application
        check_hresult(CoInitializeSecurity(
            nullptr,
            -1,
            nullptr,
            nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE,
            nullptr));

        // Adapted from https://learn.microsoft.com/en-us/windows/win32/wmisdk/creating-a-connection-to-a-wmi-namespace
        // and
        // https://learn.microsoft.com/en-us/windows/win32/wmisdk/accessing-performance-data-in-c--?redirectedfrom=MSDN
        m_locator = winrt::create_instance<IWbemLocator>(CLSID_WbemLocator);

        check_hresult(
            m_locator
                ->ConnectServer(BSTR(L"\\\\.\\root\\cimv2"), nullptr, nullptr, 0, 0, nullptr, 0, m_services.put()));

        // Adapter from
        // https://learn.microsoft.com/en-us/windows/win32/wmisdk/setting-the-security-levels-on-a-wmi-connection
        check_hresult(CoSetProxyBlanket(
            m_services.get(),
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE));

        wchar_t buf[1024];
        swprintf_s(buf, L"SELECT * FROM Win32_PerfRawData_PerfProc_Process WHERE IDProcess=%u", GetCurrentProcessId());

        com_ptr<IEnumWbemClassObject> enumerator;

        wil::unique_bstr queryLanguage = wil::make_bstr_failfast(L"WQL");
        auto query = wil::make_bstr_failfast(buf);

        check_hresult(m_services->ExecQuery(
            queryLanguage.get(),
            query.get(),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            enumerator.put()));

        ULONG result = 0;
        com_ptr<IWbemClassObject> clsObject;

        while (check_hresult(enumerator->Next(WBEM_INFINITE, 1, clsObject.put(), &result)), result != 0)
        {
            m_refresher = winrt::create_instance<IWbemRefresher>(CLSID_WbemRefresher);

            com_ptr<IWbemConfigureRefresher> refresherConfig;
            refresherConfig = m_refresher.try_as<IWbemConfigureRefresher>();

            long hiPerfId;

            check_hresult(
                refresherConfig
                    ->AddObjectByTemplate(m_services.get(), clsObject.get(), 0, nullptr, m_perfData.put(), &hiPerfId));

            // we only expect one process to have our PID!
            break;
        }

        if (m_perfData)
        {
            m_workerThread = std::thread([this] { this->WorkerThread(); });
        }
    }

    ~CpuPerformanceMonitor()
    {
        if (m_workerThread.joinable())
        {
            std::unique_lock lock(m_mutex);
            m_quit = true;
            lock.unlock();

            m_cv.notify_all();

            m_workerThread.join();
        }
    }

    void Reset()
    {
        std::unique_lock lock(m_mutex);
        m_maxPercentProcessorTime = 0.0f;
    }

    float GetMaxCpuUsage()
    {
        std::unique_lock lock(m_mutex);
        return m_maxPercentProcessorTime;
    }

private:
    void WorkerThread()
    {
        try
        {
            std::unique_lock lock(m_mutex);

            while (!m_quit)
            {
                using namespace std::chrono_literals;

                m_cv.wait_for(lock, 100ms);

                m_refresher->Refresh(0);

                wil::unique_variant percentProcessorTimeString;
                check_hresult(
                    m_perfData->Get(L"PercentProcessorTime", 0, percentProcessorTimeString.addressof(), nullptr, nullptr));

                wil::unique_variant timestampPerfTimeString;
                check_hresult(
                    m_perfData->Get(L"Timestamp_PerfTime", 0, timestampPerfTimeString.addressof(), nullptr, nullptr));

                if (percentProcessorTimeString.vt != VT_EMPTY && timestampPerfTimeString.vt != VT_EMPTY)
                {
                    uint64_t timestampPerfTime = _wtoi64(timestampPerfTimeString.bstrVal);
                    uint64_t percentProcessorTime = _wtoi64(percentProcessorTimeString.bstrVal);

                    if (m_lastTimestampPerfTime != 0)
                    {
                        int64_t timestamp_diff = timestampPerfTime - m_lastTimestampPerfTime;
                        int64_t processor_diff = percentProcessorTime - m_lastPercentProcessorTime;

                        float usage = static_cast<float>(processor_diff) / static_cast<float>(timestamp_diff);

                        m_maxPercentProcessorTime = std::max(usage, m_maxPercentProcessorTime);
                    }

                    m_lastTimestampPerfTime = timestampPerfTime;
                    m_lastPercentProcessorTime = percentProcessorTime;
                }
            }
        }
        catch (...)
        {
            Utility::Printf("Error processing cpu performance data 0x%8x\n", winrt::to_hresult());
        }
    }
};

static std::optional<CpuPerformanceMonitor> g_cpuPerformanceMonitor;

void InitializeCpuPerformanceMonitor()
{
    try
    {
        g_cpuPerformanceMonitor.emplace();
    }
    catch (...)
    {
        Utility::Printf("InitializeCpuPerformanceMonitor failed: %08x\n", winrt::to_hresult());
        // Other than the debug message, we fail silently, and just don't have CPU perf
    }
}

void ShutdownCpuPerformanceMonitor()
{
    g_cpuPerformanceMonitor.reset();
}

void ResetCpuPerformance()
{
    if (g_cpuPerformanceMonitor)
        g_cpuPerformanceMonitor->Reset();
}

float GetMaxCpuUsage()
{
    if (g_cpuPerformanceMonitor)
        return g_cpuPerformanceMonitor->GetMaxCpuUsage();

    return 0.0f;
}
