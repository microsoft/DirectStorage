/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 *
 * Advanced Technology Group (ATG)
 * Author(s):   Pavel Martishevsky (pamartis@microsoft.com)
 */

#include <stdint.h>
#include <stdio.h>

#include <winsdkver.h>
#define _WIN32_WINNT 0x0A00
#include <sdkddkver.h>

#define NOMINMAX
#define NODRAWTEXT
#define NOGDI
#define NOBITMAP
#define NOMCX
#define NOSERVICE
#define NOHELP
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>

#include "zstdgpu_assert.h"

#define D3D12AID_CHECK(call)                            \
    do                                                  \
    {                                                   \
        HRESULT hr = call;                              \
        ZSTDGPU_ASSERT_MSG(S_OK == hr, "S_OK != 0x%08lx " #call "\n", hr); \
    }                                                   \
    while(0)

#define D3D12AID_ASSERT(cond) ZSTDGPU_ASSERT(cond)

#define D3D12AID_CMD_QUEUE_LATENCY_FRAME_MAX_COUNT 2
#define D3D12AID_API_STATIC 1
#include <d3d12aid.h>

#include "platform/zstdgpu_demo_platform.h"

static bool Gd3dDbg = false;
static DWORD Gd3d12MsgCookie = 0;
static ID3D12InfoQueue *Gd3d12InfoQueue = NULL;
static void  *Gd3d12InfoQueueMemory = NULL;
static size_t Gd3d12InfoQueueMemoryByteCount = 0;

static const wchar_t *zstdgpu_Demo_D3D12SeverityToWide(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity)
    {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: return L"FAIL";
        case D3D12_MESSAGE_SEVERITY_ERROR:      return L"FAIL";
        case D3D12_MESSAGE_SEVERITY_WARNING:    return L"FAIL";
        case D3D12_MESSAGE_SEVERITY_INFO:       return L"INFO";
        case D3D12_MESSAGE_SEVERITY_MESSAGE:    return L"INFO";
        default:                                return L"INFO";
    }
}

static void CALLBACK zstdgpu_Demo_D3D12MessageCallback(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR pDescription, void *pContext)
{
    (void)category;
    (void)id;
    (void)pContext;

    wprintf(L"[%ls] %hs\n", zstdgpu_Demo_D3D12SeverityToWide(severity), pDescription);
}

ID3D12Device *zstdgpu_Demo_PlatformInit(uint32_t gpuVenId, uint32_t gpuDevId, bool d3dDbg, bool d3dGbv)
{
    // Default main thread to CPU 0
    SetThreadAffinityMask(GetCurrentThread(), 0x1);
    Gd3dDbg = d3dDbg;

    if (Gd3dDbg)
    {
        #define LOAD_F(funName, libName)                        \
            decltype(funName) *fn##funName = NULL;              \
            do                                                  \
            {                                                   \
                HMODULE lib = GetModuleHandleW(libName);        \
                if (NULL == lib)                                \
                    lib = LoadLibraryW(libName);                \
                if (NULL != lib)                                \
                    fn##funName = (decltype(funName) *)GetProcAddress(lib, #funName);\
            }                                                   \
            while(0)

        LOAD_F(DXGIGetDebugInterface, L"dxgidebug.dll");
        LOAD_F(D3D12GetDebugInterface, L"d3d12.dll");

        if (NULL != fnD3D12GetDebugInterface)
        {
            ID3D12Debug1 *d3d12Debug1 = NULL;
            D3D12AID_CHECK(fnD3D12GetDebugInterface(D3D12AID_IID_PPV_ARGS(&d3d12Debug1)));
            d3d12Debug1->EnableDebugLayer();
            if (d3dGbv)
            {
                d3d12Debug1->SetEnableGPUBasedValidation(TRUE);
            }
            //d3d12Debug1->SetEnableSynchronizedCommandQueueValidation(FALSE /* otherwise enabled by default */);
            D3D12AID_SAFE_RELEASE(d3d12Debug1);
        }

        if (NULL != fnDXGIGetDebugInterface)
        {
            BOOL isDbg = IsDebuggerPresent();

            IDXGIDebug1 *dxgiDebug1 = NULL;
            D3D12AID_CHECK(fnDXGIGetDebugInterface(D3D12AID_IID_PPV_ARGS(&dxgiDebug1)));
            dxgiDebug1->EnableLeakTrackingForThread();
            D3D12AID_SAFE_RELEASE(dxgiDebug1);

            IDXGIInfoQueue *dxgiInfoQueue = NULL;
            D3D12AID_CHECK(fnDXGIGetDebugInterface(D3D12AID_IID_PPV_ARGS(&dxgiInfoQueue)));
            dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, isDbg);
            dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, isDbg);
            dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING, isDbg);
            dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_MESSAGE, isDbg);
            dxgiInfoQueue->ClearStorageFilter(DXGI_DEBUG_ALL);
            dxgiInfoQueue->ClearRetrievalFilter(DXGI_DEBUG_ALL);
            D3D12AID_SAFE_RELEASE(dxgiInfoQueue);
        }
    }

    IDXGIFactory6 *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    ID3D12Device *device = NULL;
    D3D12AID_CHECK(CreateDXGIFactory2(0, D3D12AID_IID_PPV_ARGS(&factory)));
    for (unsigned i = 0;
         DXGI_ERROR_NOT_FOUND != factory->EnumAdapterByGpuPreference(
                                     i,
                                     DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                     D3D12AID_IID_PPV_ARGS(&adapter));
         ++i)
    {
        DXGI_ADAPTER_DESC desc;
        D3D12AID_CHECK(adapter->GetDesc(&desc));

        bool matchDevId = ~0u == gpuDevId || gpuDevId == desc.DeviceId; /** Consider DeviceId a match if requested one is ~0 meaning 'dont care' or == desc.DeviceId */
        bool firstVenId = gpuVenId != desc.VendorId && 0x1414 == gpuVenId; /** This condition may seem strange, but when gpuVenId is set to 0x1414 -- it means find anything except 0x1414 */
        bool matchVenId = gpuVenId == desc.VendorId;

        if ((firstVenId && matchDevId) || (matchVenId && matchDevId))
        {
            D3D12AID_CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, D3D12AID_IID_PPV_ARGS(&device)));
            adapter->Release();
            factory->Release();

            if (Gd3dDbg)
            {
                ID3D12InfoQueue *d3d12InfoQueue = NULL;
#ifdef  __ID3D12InfoQueue1_INTERFACE_DEFINED__
                ID3D12InfoQueue1 *d3d12InfoQueue1 = NULL;
                if (S_OK == device->QueryInterface(D3D12AID_IID_PPV_ARGS(&d3d12InfoQueue1)))
                {
                    d3d12InfoQueue = d3d12InfoQueue1;
                }
#endif
                if (NULL == d3d12InfoQueue)
                {
                    D3D12AID_CHECK(device->QueryInterface(D3D12AID_IID_PPV_ARGS(&d3d12InfoQueue)));
                    Gd3d12InfoQueue = d3d12InfoQueue;
                }
                BOOL isDbg = IsDebuggerPresent();

                d3d12InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, isDbg);
                d3d12InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, isDbg);
                d3d12InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, isDbg);
                d3d12InfoQueue->ClearRetrievalFilter();
                d3d12InfoQueue->ClearStorageFilter();
                d3d12InfoQueue->SetMuteDebugOutput(FALSE);

                // Disable State Creation message category
                D3D12_MESSAGE_CATEGORY disableCategoryList [] = { D3D12_MESSAGE_CATEGORY_STATE_CREATION };
                D3D12_INFO_QUEUE_FILTER filter = {};
                filter.DenyList.pCategoryList = disableCategoryList;
                filter.DenyList.NumCategories = _countof(disableCategoryList);
                d3d12InfoQueue->PushStorageFilter(&filter);
                d3d12InfoQueue->PushRetrievalFilter(&filter);

#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
                if (NULL != d3d12InfoQueue1)
                {
                    d3d12InfoQueue1->RegisterMessageCallback(zstdgpu_Demo_D3D12MessageCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &Gd3d12MsgCookie);
                    D3D12AID_SAFE_RELEASE(d3d12InfoQueue1);
                }
#endif
            }
            return device;
        }
        adapter->Release();
    }
    factory->Release();
    return NULL;
}

void zstdgpu_Demo_PlatformTerm(struct ID3D12Device *device)
{
    if (Gd3dDbg)
    {
        D3D12AID_SAFE_RELEASE(Gd3d12InfoQueue);
        if (NULL != Gd3d12InfoQueueMemory)
        {
            free(Gd3d12InfoQueueMemory);
            Gd3d12InfoQueueMemory = NULL;
            Gd3d12InfoQueueMemoryByteCount = 0;
        }

        ID3D12DebugDevice1 *d3d12DebugDevice1 = NULL;
        //ID3D12InfoQueue1 *d3d12InfoQueue1 = NULL;
        device->QueryInterface(D3D12AID_IID_PPV_ARGS(&d3d12DebugDevice1));
        //device->QueryInterface(D3D12AID_IID_PPV_ARGS(&d3d12InfoQueue1));
        D3D12AID_SAFE_RELEASE(device);

        d3d12DebugDevice1->ReportLiveDeviceObjects(D3D12_RLDO_SUMMARY | D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        D3D12AID_SAFE_RELEASE(d3d12DebugDevice1);

        // NOTE(pamartis): Intentionally don't pop the filters so various instrumentation resources aren't reported
        //d3d12InfoQueue1->PopStorageFilter();
        //d3d12InfoQueue1->PopRetrievalFilter();

#if 0   // NOTE(pamartis): Intentionally don't remove the callback as it keeps getting messages even after the release of the queue
        if (0 != Gd3d12MsgCookie)
        {
            d3d12InfoQueue1->UnregisterMessageCallback(Gd3d12MsgCookie);
            Gd3d12MsgCookie = 0;
        }
#endif
        //D3D12AID_SAFE_RELEASE(d3d12InfoQueue1);

        IDXGIDebug *dxgiDebug = NULL;
        IDXGIInfoQueue *dxgiInfoQueue = NULL;
        LOAD_F(DXGIGetDebugInterface, L"dxgidebug.dll");
        if (NULL != fnDXGIGetDebugInterface)
        {
            D3D12AID_CHECK(fnDXGIGetDebugInterface(D3D12AID_IID_PPV_ARGS(&dxgiDebug)));
            D3D12AID_CHECK(fnDXGIGetDebugInterface(D3D12AID_IID_PPV_ARGS(&dxgiInfoQueue)));
            D3D12AID_CHECK(dxgiInfoQueue->PushEmptyRetrievalFilter(DXGI_DEBUG_ALL));
            D3D12AID_CHECK(dxgiInfoQueue->PushEmptyStorageFilter(DXGI_DEBUG_ALL));
            dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
        }
        D3D12AID_SAFE_RELEASE(dxgiInfoQueue);
        D3D12AID_SAFE_RELEASE(dxgiDebug);
    }
    else
    {
        D3D12AID_SAFE_RELEASE(device);
    }
}

uint32_t zstdgpu_Demo_PlatformTick(void)
{
    if (NULL != Gd3d12InfoQueue)
    {
        const uint64_t msgCnt = Gd3d12InfoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (uint64_t i = 0; i < msgCnt; ++i)
        {
            size_t msgByteCnt = 0;
            if (S_OK == Gd3d12InfoQueue->GetMessageW(i, NULL, &msgByteCnt))
            {
                if (msgByteCnt > Gd3d12InfoQueueMemoryByteCount)
                {
                    free(Gd3d12InfoQueueMemory);
                    Gd3d12InfoQueueMemory = NULL;
                    Gd3d12InfoQueueMemoryByteCount = 0;
                }
                if (NULL == Gd3d12InfoQueueMemory)
                {
                    Gd3d12InfoQueueMemory = malloc(msgByteCnt);
                    Gd3d12InfoQueueMemoryByteCount = msgByteCnt;
                }
                if (NULL != Gd3d12InfoQueueMemory)
                {
                    D3D12_MESSAGE* msg = (D3D12_MESSAGE *)Gd3d12InfoQueueMemory;
                    if (S_OK == Gd3d12InfoQueue->GetMessageW(i, msg, &msgByteCnt))
                    {
                        zstdgpu_Demo_D3D12MessageCallback(msg->Category, msg->Severity, msg->ID, msg->pDescription, NULL);
                    }
                }
            }
        }
        Gd3d12InfoQueue->ClearStoredMessages();
    }
    return 1;
}
