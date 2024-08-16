//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#define NOMINMAX

#include "CpuPerformance.h"
#include "DStorageLoader.h"
#include "MarcFile.h"
#include "MarcFileManager.h"

#include <BufferManager.h>
#include <FXAA.h>
#include <GameCore.h>
#include <GameInput.h>
#include <PostEffects.h>
#include <Renderer.h>
#include <SSAO.h>
#include <ShadowCamera.h>

#include <optional>
#include <random>

using namespace Math;

class BulkLoadDemo : public GameCore::IGameApp
{
public:
    BulkLoadDemo()
    {
        GameInput::DisableMouse = true;
    }

    void Startup() override;
    void Cleanup() override;

    void Update(float deltaT) override;
    void RenderScene() override;
    void RenderUI(GraphicsContext& gfxContext) override;

private:
    void LoadIblTextures(std::filesystem::path const& directory);

    void LoadNextSet();
    void ShowSet();

    void UpdateInstances(float deltaT);
    void RenderInstances(Renderer::MeshSorter& sorter);

    std::default_random_engine m_rng;

    bool m_enableGpuDecompression = true;

    std::optional<MarcFileManager> m_marcFiles;

    std::vector<MarcFileManager::FileId> m_fileIds;

    struct Object
    {
        ModelInstance ModelInstance;
        Vector3 TumbleAxis;
        Vector3 StartPos;
    };

    std::vector<Object> m_objects;
    float m_t;
    BoundingSphere m_objectsBoundingSphere;

    enum class State
    {
        Idle,
        LoadingASet,
        ShowingASet,
        Unloading
    };

    State m_state{State::Idle};

    float m_maxCpuUsage = 0;

    uint64_t m_lastObjectRenderFenceValue = static_cast<uint64_t>(-1);

    Camera m_camera;
    ShadowCamera m_sunShadowCamera;
};

CREATE_APPLICATION(BulkLoadDemo);

namespace EngineProfiling
{
    extern BoolVar DrawFrameRate;
}

static std::filesystem::path GetExecutableDirectory()
{
    std::wstring moduleFilename;
    moduleFilename.resize(MAX_PATH);

    size_t pathLength;
    while (pathLength = GetModuleFileNameW(nullptr, moduleFilename.data(), (DWORD)moduleFilename.size()),
           pathLength == moduleFilename.size())
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            std::abort();

        moduleFilename.resize(moduleFilename.size() * 2);
    }
    moduleFilename.resize(pathLength);

    return std::filesystem::path(moduleFilename).remove_filename();
}

void BulkLoadDemo::Startup()
{
    InitializeCpuPerformanceMonitor();

    FXAA::Enable = false;
    PostEffects::EnableHDR = true;
    PostEffects::EnableAdaptation = true;
    SSAO::Enable = true;
    EngineProfiling::DrawFrameRate = false;

    uint32_t enableGpuDecompression = 1;
    CommandLineArgs::GetInteger(L"gpu-decompression", enableGpuDecompression);

    m_enableGpuDecompression = !!enableGpuDecompression;

    InitializeDStorage(!m_enableGpuDecompression);
    Renderer::Initialize();

    m_camera.SetZRange(1.0f, 10000.0f);

    std::filesystem::path executableDirectory = GetExecutableDirectory();
    LoadIblTextures(executableDirectory);

    // Construct the MarcFileManager.  This is deferred until after the renderer
    // has been initialized.
    m_marcFiles.emplace();

    // Figure out what mode we're running in, and add the appropriate files.
    std::vector<std::wstring> filesToLoad;

    std::wstring param;
    if (CommandLineArgs::GetString(L"model", param))
    {
        filesToLoad.push_back(std::move(param));
    }
    else if (CommandLineArgs::GetString(L"dir", param))
    {
        for (auto const& entry : std::filesystem::directory_iterator(param))
        {
            if (entry.is_regular_file())
            {
                auto fname = entry.path();

                fname.make_preferred();

                if (fname.extension() == ".marc")
                {
                    filesToLoad.push_back(fname.wstring());
                }
            }
        }
    }
    else
    {
        // By default look for .marc files next to the executable
        std::vector<std::wstring> localMarcFiles;

        for (auto const& entry : std::filesystem::directory_iterator(executableDirectory))
        {
            if (entry.is_regular_file())
            {
                auto fname = entry.path();

                fname.make_preferred();

                if (fname.extension() == ".marc")
                {
                    localMarcFiles.push_back(fname);
                }
            }
        }

        // load multiple instances of these files
        constexpr size_t numInstances = 1024;
        for (size_t i = 0; i < std::max<size_t>(1, numInstances / localMarcFiles.size()); ++i)
        {
            for (auto& f : localMarcFiles)
            {
                filesToLoad.push_back(f);
            }
        }
    }

    // Add all the files
    for (auto& f : filesToLoad)
    {
        m_fileIds.push_back(m_marcFiles->Add(f));
    }
}

void BulkLoadDemo::Cleanup()
{
    m_marcFiles.reset();

    Renderer::Shutdown();
    ShutdownDStorage();

    ShutdownCpuPerformanceMonitor();
}

void BulkLoadDemo::LoadIblTextures(std::filesystem::path const& directory)
{
    std::filesystem::path texturesDirectory = directory / L"Textures";

    TextureRef diffuse = TextureManager::LoadDDSFromFile((texturesDirectory / L"Stonewall_diffuseIBL.dds").wstring());
    TextureRef specular = TextureManager::LoadDDSFromFile((texturesDirectory / L"Stonewall_specularIBL.dds").wstring());

    Renderer::SetIBLTextures(diffuse, specular);
}

namespace Graphics
{
    extern EnumVar DebugZoom;
}

void BulkLoadDemo::Update(float deltaT)
{
    ScopedTimer _prof(L"Update State");

    if (GameInput::IsFirstPressed(GameInput::kLShoulder))
        Graphics::DebugZoom.Decrement();
    else if (GameInput::IsFirstPressed(GameInput::kRShoulder))
        Graphics::DebugZoom.Increment();

    m_marcFiles->Update();

    using namespace std::chrono_literals;

    switch (m_state)
    {
    case State::Idle:
        if (m_marcFiles->IsReadyToLoad())
        {
            LoadNextSet();
            m_state = State::LoadingASet;
        }
        break;

    case State::LoadingASet:
        if (m_marcFiles->SetIsLoaded())
        {
            ShowSet();
            m_state = State::ShowingASet;
        }
        break;

    case State::ShowingASet:
        if ((m_marcFiles->GetTimeSinceLoad() - m_marcFiles->GetLoadTime()) > 10s)
        {
            m_state = State::Unloading;
        }
        break;

    case State::Unloading:
        // Unloading is surprisingly expensive - especially freeing the mesh
        // constants buffers associated with ModelInstances (these are all committed
        // resources).  So we +1 to the fence value so that we are sure we do this
        // when the loading screen is visible, which hides the glitch.
        if (Graphics::g_CommandManager.GetQueue().IsFenceComplete(m_lastObjectRenderFenceValue + 1))
        {
            m_objects.clear();
            m_marcFiles->UnloadSet();

            m_state = State::Idle;
            m_lastObjectRenderFenceValue = static_cast<uint64_t>(-1);
        }
        break;
    }

    // Update the camera
    {
        Matrix3 orientation = Matrix3::MakeXRotation(-m_t * 0.1f);

        Vector3 position = orientation.GetZ() * (50.0f + m_t);
        m_camera.SetTransform(AffineTransform(orientation, position));
        m_camera.Update();
    }

    UpdateInstances(deltaT);
}

void BulkLoadDemo::LoadNextSet()
{
    // Shuffle the models, so that we load them in a random order each time
    std::shuffle(m_fileIds.begin(), m_fileIds.end(), m_rng);
    ResetCpuPerformance();
    m_marcFiles->SetNextSet(m_fileIds);
}

void BulkLoadDemo::ShowSet()
{
    float cpuUsage = GetMaxCpuUsage();

    static int numProcessors = []()
    {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        return sysInfo.dwNumberOfProcessors;
    }();

    m_maxCpuUsage = std::min(100.0f, 100.0f * cpuUsage / numProcessors);

    auto instances = m_marcFiles->CreateInstancesForSet();

    constexpr float instanceRadius = 10.0f;

    auto numColumns = static_cast<int>((instances.size() + 1) / 2);

    int instanceIndex = 0;

    for (auto& instance : instances)
    {
        Object object{};
        object.ModelInstance = std::move(instance);
        object.ModelInstance.LoopAllAnimations();

        int row = instanceIndex / numColumns;
        int column = instanceIndex % numColumns;

        object.StartPos = Vector3(column * instanceRadius * 2.0f, 0, -row * instanceRadius * 3.0f);

        std::uniform_real_distribution<float> d(0.01f, 2.0f);
        object.TumbleAxis = Vector3(0, d(m_rng), 0);

        ++instanceIndex;

        m_objects.push_back(std::move(object));
    }

    m_t = 0;
}

void BulkLoadDemo::UpdateInstances(float deltaT)
{
    using namespace DirectX;

    m_objectsBoundingSphere = BoundingSphere(EZeroTag{});

    GraphicsContext& gfxContext = GraphicsContext::Begin(L"UpdateInstances");

    for (auto& object : m_objects)
    {
        object.ModelInstance.Locator() = UniformTransform(EIdentityTag{});
        auto boundingSphere = object.ModelInstance.GetBoundingSphere();
        auto center = boundingSphere.GetCenter();

        float x = -m_t * 50.0f;

        XMMATRIX transform = XMMatrixTranslationFromVector(-center) *
                             XMMatrixScalingFromVector(Scalar(10.0f) / boundingSphere.GetRadius()) *
                             XMMatrixRotationAxis(object.TumbleAxis, m_t * (float)Length(object.TumbleAxis)) *
                             XMMatrixTranslationFromVector(object.StartPos) * XMMatrixTranslation(x, 0, 0);

        XMVECTOR decomposedScale;
        XMVECTOR decomposedRotationQuat;
        XMVECTOR decomposedTranslate;
        XMMatrixDecompose(&decomposedScale, &decomposedRotationQuat, &decomposedTranslate, transform);

        object.ModelInstance.Locator() = UniformTransform(
            Quaternion(decomposedRotationQuat),
            Scalar(XMVectorSwizzle<XM_SWIZZLE_X, XM_SWIZZLE_X, XM_SWIZZLE_X, XM_SWIZZLE_X>(decomposedScale)),
            Vector3(decomposedTranslate));

        if (m_state == State::ShowingASet)
            object.ModelInstance.Update(gfxContext, deltaT);

        if (m_objectsBoundingSphere.GetRadius() == 0.0f)
        {
            m_objectsBoundingSphere = object.ModelInstance.GetBoundingSphere();
        }
        else
        {
            m_objectsBoundingSphere = m_objectsBoundingSphere.Union(object.ModelInstance.GetBoundingSphere());
        }
    }

    gfxContext.Finish();

    m_t += deltaT;
}

void BulkLoadDemo::RenderScene()
{
    using namespace Graphics;
    using Renderer::MeshSorter;

    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)Graphics::g_SceneColorBuffer.GetWidth();
    viewport.Height = (float)Graphics::g_SceneColorBuffer.GetHeight();
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = (LONG)Graphics::g_SceneColorBuffer.GetWidth();
    scissor.bottom = (LONG)Graphics::g_SceneColorBuffer.GetHeight();

    // Update global constants
    constexpr float sunOrientation = -0.5f;
    constexpr float sunInclination = 0.75f;

    float costheta = cosf(sunOrientation);
    float sintheta = sinf(sunOrientation);
    float cosphi = cosf(sunInclination * 3.14159f * 0.5f);
    float sinphi = sinf(sunInclination * 3.14159f * 0.5f);

    Vector3 SunDirection = Normalize(Vector3(costheta * cosphi, sinphi, sintheta * cosphi));
    Vector3 ShadowBounds = Vector3(m_objectsBoundingSphere.GetRadius());

    m_sunShadowCamera.UpdateMatrix(
        -SunDirection,
        Vector3(0, -500.0f, 0),
        Vector3(5000, 3000, 3000),
        (uint32_t)g_ShadowBuffer.GetWidth(),
        (uint32_t)g_ShadowBuffer.GetHeight(),
        16);

    GlobalConstants globals;
    globals.ViewProjMatrix = m_camera.GetViewProjMatrix();
    globals.SunShadowMatrix = m_sunShadowCamera.GetShadowMatrix();
    globals.CameraPos = m_camera.GetPosition();
    globals.SunDirection = SunDirection;
    globals.SunIntensity = Vector3(Scalar(4.0f));

    // Begin rendering depth
    gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
    gfxContext.ClearDepth(g_SceneDepthBuffer);

    MeshSorter sorter(MeshSorter::kDefault);
    sorter.SetCamera(m_camera);
    sorter.SetViewport(viewport);
    sorter.SetScissor(scissor);
    sorter.SetDepthStencilTarget(g_SceneDepthBuffer);
    sorter.AddRenderTarget(g_SceneColorBuffer);

    RenderInstances(sorter);

    sorter.Sort();

    {
        ScopedTimer _prof(L"Depth Pre-Pass", gfxContext);
        sorter.RenderMeshes(MeshSorter::kZPass, gfxContext, globals);
    }

    SSAO::Render(gfxContext, m_camera);

    if (!SSAO::DebugDraw)
    {
        ScopedTimer _outerprof(L"Main Render", gfxContext);

        {
            ScopedTimer _prof(L"Sun Shadow Map", gfxContext);

            MeshSorter shadowSorter(MeshSorter::kShadows);
            shadowSorter.SetCamera(m_sunShadowCamera);
            shadowSorter.SetDepthStencilTarget(g_ShadowBuffer);

            RenderInstances(shadowSorter);

            shadowSorter.Sort();
            shadowSorter.RenderMeshes(MeshSorter::kZPass, gfxContext, globals);
        }

        gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        gfxContext.ClearColor(g_SceneColorBuffer);

        {
            ScopedTimer _prof(L"Render Color", gfxContext);

            gfxContext.TransitionResource(g_SSAOFullScreen, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
            gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV(), g_SceneDepthBuffer.GetDSV_DepthReadOnly());
            gfxContext.SetViewportAndScissor(viewport, scissor);

            sorter.RenderMeshes(MeshSorter::kOpaque, gfxContext, globals);
        }

        if (m_state == State::ShowingASet)
        {
            ScopedTimer _prof(L"DrawSkybox");
            Renderer::DrawSkybox(gfxContext, m_camera, viewport, scissor);
        }

        {
            ScopedTimer _prof(L"Render Transparent");
            sorter.RenderMeshes(MeshSorter::kTransparent, gfxContext, globals);
        }
    }

    gfxContext.Finish();
}

void BulkLoadDemo::RenderInstances(Renderer::MeshSorter& sorter)
{
    if (m_state != State::ShowingASet)
        return;

    for (auto& object : m_objects)
    {
        object.ModelInstance.Render(sorter);
    }

    m_lastObjectRenderFenceValue = Graphics::g_CommandManager.GetQueue().GetNextFenceValue();
}

namespace EngineTuning
{
    extern bool sm_IsVisible;
}

void BulkLoadDemo::RenderUI(GraphicsContext& gfxContext)
{
    if (EngineTuning::sm_IsVisible)
        return;

    TextContext text(gfxContext);
    text.Begin();

    text.ResetCursor(0.0f, 1.0f);

    if (m_state != State::ShowingASet)
    {
        text.SetTextSize(34.0f);
        text.EnableDropShadow(true);

        if (m_marcFiles->IsLoading())
        {
            auto const& s = m_marcFiles->GetCurrentSetSize();
            text.DrawFormattedString(
                "%.2f GB loaded in %.2f seconds\n%6.2f%% Max CPU usage\n",
                (s.CpuByteCount + s.TexturesByteCount + s.BuffersByteCount) / 1000.0f / 1000.0f / 1000.0f,
                m_marcFiles->GetTimeSinceLoad().count(),
                m_maxCpuUsage);
        }
    }
    else
    {
        auto s = m_marcFiles->GetCurrentSetSize();
        auto time = m_marcFiles->GetLoadTime();

        size_t total = s.CpuByteCount + s.TexturesByteCount + s.BuffersByteCount;

        text.SetTextSize(34.0f);
        text.DrawFormattedString(
            "%.2f GB loaded in %.2f seconds\n%6.2f%% Max CPU usage\n",
            total / 1000.0f / 1000.0f / 1000.0f,
            time.count(),
            m_maxCpuUsage);
        text.SetTextSize(24.0f);
        text.NewLine();

        text.DrawFormattedString("   Bandwidth: %7.2f GB/s\n", (total / time.count()) / 1000.0f / 1000.0f / 1000.0f);
        text.DrawFormattedString("CPU Mem Data: %7.2f MiB\n", s.CpuByteCount / 1024.0f / 1024.0f);
        text.DrawFormattedString("Texture Data: %7.2f MiB\n", s.TexturesByteCount / 1024.0f / 1024.0f);
        text.DrawFormattedString(" Buffer Data: %7.2f MiB\n\n", s.BuffersByteCount / 1024.0f / 1024.0f);

        if (s.UncompressedByteCount > 0)
            text.DrawFormattedString("Uncompressed: %7.2f MB\n", s.UncompressedByteCount / 1000.0f / 1000.0f);

        if (s.GDeflateByteCount > 0)
            text.DrawFormattedString(
                "    GDeflate: %7.2f MB (%s decompression)\n",
                s.GDeflateByteCount / 1000.0f / 1000.0f,
                m_enableGpuDecompression ? "GPU" : "CPU");

        if (s.ZLibByteCount > 0)
            text.DrawFormattedString("        Zlib: %7.2f MB\n", s.ZLibByteCount / 1000.0f / 1000.0f);

        text.NewLine();

        text.DrawFormattedString("              %7u models\n", s.NumLoadedModels);
        text.DrawFormattedString("              %7d textures\n", s.NumTextureHandles);
    }

    text.End();
}