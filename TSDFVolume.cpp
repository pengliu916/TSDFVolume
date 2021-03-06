#include "pch.h"
#include "TSDFVolume.h"

using namespace DirectX;
using namespace Microsoft::WRL;
using namespace std;

#define frand() ((float)rand()/RAND_MAX)
namespace {
    const DXGI_FORMAT _stepInfoTexFormat = DXGI_FORMAT_R16G16_FLOAT;
    bool _typedLoadSupported = false;

    bool _useStepInfoTex = false;
    bool _stepInfoDebug = false;
    bool _blockVolumeUpdate = false;
    bool _resetVolume = true;
    bool _useQuadRaycast = false;

    // define the geometry for a triangle.
    const XMFLOAT3 cubeVertices[] = {
        {XMFLOAT3(-0.5f, -0.5f, -0.5f)},{XMFLOAT3(-0.5f, -0.5f,  0.5f)},
        {XMFLOAT3(-0.5f,  0.5f, -0.5f)},{XMFLOAT3(-0.5f,  0.5f,  0.5f)},
        {XMFLOAT3(0.5f, -0.5f, -0.5f)},{XMFLOAT3(0.5f, -0.5f,  0.5f)},
        {XMFLOAT3(0.5f,  0.5f, -0.5f)},{XMFLOAT3(0.5f,  0.5f,  0.5f)},
    };
    // the index buffer for triangle strip
    const uint16_t cubeTrianglesStripIndices[CUBE_TRIANGLESTRIP_LENGTH] = {
        6, 4, 2, 0, 1, 4, 5, 7, 1, 3, 2, 7, 6, 4
    };
    // the index buffer for cube wire frame (0xffff is the cut value set later)
    const uint16_t cubeLineStripIndices[CUBE_LINESTRIP_LENGTH] = {
        0, 1, 5, 4, 0, 2, 3, 7, 6, 2, 0xffff, 6, 4, 0xffff, 7, 5, 0xffff, 3, 1
    };

    std::once_flag _psoCompiled_flag;
    RootSignature _rootsig;
    ComputePSO _cptUpdatePSO[ManagedBuf::kNumType][TSDFVolume::kNumStruct][2];
    GraphicsPSO _gfxRenderPSO[ManagedBuf::kNumType]
        [TSDFVolume::kNumStruct][TSDFVolume::kNumFilter][2];
    GraphicsPSO _gfxStepInfoPSO;
    GraphicsPSO _gfxStepInfoDebugPSO;
    ComputePSO _cptFlagVolResetPSO;
    ComputePSO _cptCreateBlockQueuePSO[2];
    ComputePSO _cptBlockQueueResolvePSO;
    ComputePSO _cptBlockVolumeUpdatePSO[ManagedBuf::kNumType]
        [TSDFVolume::kNumStruct][2];
    ComputePSO _cptTSDFBufResetPSO[ManagedBuf::kNumType];
    StructuredBuffer _cubeVB;
    ByteAddressBuffer _cubeTriangleStripIB;
    ByteAddressBuffer _cubeLineStripIB;

    BOOLEAN GetBaseAndPower2(uint16_t input, uint16_t& base, uint16_t& power2)
    {
        uint32_t index;
        BOOLEAN isZero = _BitScanReverse((unsigned long*)&index, input);
        power2 = (uint16_t)index;
        base = input >> power2;
        return isZero;
    }

    void _BuildBrickRatioVector(uint16_t minDimension,
        std::vector<uint16_t>& ratios)
    {
        uint16_t base, power2;
        GetBaseAndPower2(minDimension, base, power2);
        ASSERT(power2 > 3);
        ratios.clear();
        for (uint16_t i = 3; i <  power2 - 1; ++i) {
            ratios.push_back(1 << i);
        }
    }

    inline bool _IsResolutionChanged(const uint3& a, const uint3& b)
    {
        return a.x != b.x || a.y != b.y || a.z != b.z;
    }

    inline HRESULT _Compile(LPCWSTR fileName, LPCSTR target,
        const D3D_SHADER_MACRO* macro, ID3DBlob** bolb)
    {
        return Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
            fileName).c_str(), macro,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bolb);
    }

    void _CreatePSOs()
    {
        HRESULT hr;
        // Compile Shaders
        ComPtr<ID3DBlob>
            volUpdateCS[ManagedBuf::kNumType][TSDFVolume::kNumStruct][2];
        ComPtr<ID3DBlob>
            blockUpdateCS[ManagedBuf::kNumType][TSDFVolume::kNumStruct][2];
        ComPtr<ID3DBlob> cubeVS, quadVS, stepInfoVS,
            volReset[ManagedBuf::kNumType];
        ComPtr<ID3DBlob> raycastPS[ManagedBuf::kNumType]
            [TSDFVolume::kNumStruct][TSDFVolume::kNumFilter];

        D3D_SHADER_MACRO macro[] = {
            {"__hlsl", "1"},//0
            {"TYPED_UAV", "0"},//1
            {"TEX3D_UAV", "0"},//2
            {"FILTER_READ", "0"},//3
            {"ENABLE_BRICKS", "0"},//4
            {"META_BALL", "0"},//5
            {"QUAD_RAYCAST", "0"},
            {nullptr, nullptr}
        };

        V(_Compile(L"TSDFVolume_RayCast_vs.hlsl", "vs_5_1", macro, &cubeVS));
        macro[6].Definition = "1";
        V(_Compile(L"TSDFVolume_RayCast_vs.hlsl", "vs_5_1", macro, &quadVS));
        macro[6].Definition = "0";

        uint DefIdx;
        for (int j = 0; j < TSDFVolume::kNumStruct; ++j) {
            macro[4].Definition = j == TSDFVolume::kVoxel ? "0" : "1";
            static bool compiledOnce = false;
            for (int i = 0; i < ManagedBuf::kNumType; ++i) {
                switch ((ManagedBuf::Type)i) {
                case ManagedBuf::kTypedBuffer: DefIdx = 1; break;
                case ManagedBuf::k3DTexBuffer: DefIdx = 2; break;
                }
                macro[DefIdx].Definition = "1";
                if (!compiledOnce) {
                    V(_Compile(L"TSDFVolume_VolumeReset_cs.hlsl", "cs_5_1",
                        macro, &volReset[i]));
                }
                V(_Compile(L"TSDFVolume_VolumeUpdate_cs.hlsl", "cs_5_1",
                    macro, &volUpdateCS[i][j][0]));
                V(_Compile(L"TSDFVolume_BlocksUpdate_cs.hlsl", "cs_5_1",
                    macro, &blockUpdateCS[i][j][0]));
                macro[5].Definition = "1";
                V(_Compile(L"TSDFVolume_VolumeUpdate_cs.hlsl", "cs_5_1",
                    macro, &volUpdateCS[i][j][1]));
                V(_Compile(L"TSDFVolume_BlocksUpdate_cs.hlsl", "cs_5_1",
                    macro, &blockUpdateCS[i][j][1]));
                macro[5].Definition = "0";
                V(_Compile(L"TSDFVolume_RayCast_ps.hlsl", "ps_5_1",
                    macro, &raycastPS[i][j][TSDFVolume::kNoFilter]));
                macro[3].Definition = "1"; // FILTER_READ
                V(_Compile(L"TSDFVolume_RayCast_ps.hlsl", "ps_5_1",
                    macro, &raycastPS[i][j][TSDFVolume::kLinearFilter]));
                macro[3].Definition = "2"; // FILTER_READ
                V(_Compile(L"TSDFVolume_RayCast_ps.hlsl", "ps_5_1",
                    macro, &raycastPS[i][j][TSDFVolume::kSamplerLinear]));
                macro[3].Definition = "0"; // FILTER_READ
                macro[DefIdx].Definition = "0";
            }
            compiledOnce = true;
        }
        // Create Rootsignature
        _rootsig.Reset(4, 1);
        _rootsig.InitStaticSampler(0, Graphics::g_SamplerLinearClampDesc);
        _rootsig[0].InitAsConstantBuffer(0);
        _rootsig[1].InitAsConstantBuffer(1);
        _rootsig[2].InitAsDescriptorRange(
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 3);
        _rootsig[3].InitAsDescriptorRange(
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 3);
        _rootsig.Finalize(L"TSDFVolume",
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

        // Define the vertex input layout.
        D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
            D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };

        // Create PSO for volume update and volume render
        DXGI_FORMAT ColorFormat = Graphics::g_SceneColorBuffer.GetFormat();
        DXGI_FORMAT DepthFormat = Graphics::g_SceneDepthBuffer.GetFormat();
        DXGI_FORMAT Tex3DFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

#define CreatePSO( ObjName, Shader)\
ObjName.SetRootSignature(_rootsig);\
ObjName.SetComputeShader(Shader->GetBufferPointer(), Shader->GetBufferSize());\
ObjName.Finalize();

        for (int k = 0; k < TSDFVolume::kNumStruct; ++k) {
            static bool compiledOnce = false;
            for (int i = 0; i < ManagedBuf::kNumType; ++i) {
                for (int j = 0; j < TSDFVolume::kNumFilter; ++j) {
                    _gfxRenderPSO[i][k][j][0].SetRootSignature(_rootsig);
                    _gfxRenderPSO[i][k][j][0].SetRasterizerState(
                        Graphics::g_RasterizerDefault);
                    _gfxRenderPSO[i][k][j][0].SetBlendState(
                        Graphics::g_BlendDisable);
                    _gfxRenderPSO[i][k][j][0].SetDepthStencilState(
                        Graphics::g_DepthStateReadWrite);
                    _gfxRenderPSO[i][k][j][0].SetSampleMask(UINT_MAX);
                    _gfxRenderPSO[i][k][j][0].SetPrimitiveTopologyType(
                        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
                    _gfxRenderPSO[i][k][j][0].SetRenderTargetFormats(
                        1, &ColorFormat, DepthFormat);
                    _gfxRenderPSO[i][k][j][0].SetPixelShader(
                        raycastPS[i][k][j]->GetBufferPointer(),
                        raycastPS[i][k][j]->GetBufferSize());
                    _gfxRenderPSO[i][k][j][1] = _gfxRenderPSO[i][k][j][0];
                    _gfxRenderPSO[i][k][j][0].SetInputLayout(
                        _countof(inputElementDescs), inputElementDescs);
                    _gfxRenderPSO[i][k][j][1].SetInputLayout(0, nullptr);
                    _gfxRenderPSO[i][k][j][1].SetVertexShader(
                        quadVS->GetBufferPointer(), quadVS->GetBufferSize());
                    _gfxRenderPSO[i][k][j][0].SetVertexShader(
                        cubeVS->GetBufferPointer(), cubeVS->GetBufferSize());
                    _gfxRenderPSO[i][k][j][0].Finalize();
                    _gfxRenderPSO[i][k][j][1].Finalize();
                }
                CreatePSO(_cptUpdatePSO[i][k][0], volUpdateCS[i][k][0]);
                CreatePSO(_cptUpdatePSO[i][k][1], volUpdateCS[i][k][1]);
                CreatePSO(_cptBlockVolumeUpdatePSO[i][k][0],
                    blockUpdateCS[i][k][0]);
                CreatePSO(_cptBlockVolumeUpdatePSO[i][k][1],
                    blockUpdateCS[i][k][1]);
                if (!compiledOnce) {
                    CreatePSO(_cptTSDFBufResetPSO[i], volReset[i]);
                }
            }
            compiledOnce = true;
        }

        // Create PSO for render near far plane
        ComPtr<ID3DBlob> stepInfoPS, stepInfoDebugPS, resetCS,
            createBlockQueueCS[2], resolveBlockQueueCS;
        D3D_SHADER_MACRO macro1[] = {
            {"__hlsl", "1"},
            {"DEBUG_VIEW", "0"},
            {"META_BALL", "0"},
            {nullptr, nullptr}
        };
        V(_Compile(L"TSDFVolume_StepInfo_cs.hlsl", "cs_5_1",
            macro1, &resetCS));
        V(_Compile(L"TSDFVolume_StepInfo_ps.hlsl", "ps_5_1",
            macro1, &stepInfoPS));
        V(_Compile(L"TSDFVolume_StepInfo_vs.hlsl", "vs_5_1",
            macro1, &stepInfoVS));
        V(_Compile(L"TSDFVolume_BlockQueueCreate_cs.hlsl", "cs_5_1",
            macro1, &createBlockQueueCS[0]));
        macro1[2].Definition = "1";
        V(_Compile(L"TSDFVolume_BlockQueueCreate_cs.hlsl", "cs_5_1",
                macro1, &createBlockQueueCS[1]));
        V(_Compile(L"TSDFVolume_BlockQueueResolve_cs.hlsl", "cs_5_1",
            macro1, &resolveBlockQueueCS));
        macro[1].Definition = "1";
        V(_Compile(L"TSDFVolume_StepInfo_ps.hlsl", "ps_5_1",
            macro1, &stepInfoDebugPS));

        // Create PSO for clean brick volume
        CreatePSO(_cptFlagVolResetPSO, resetCS);

        // Create PSO for update block volume
        CreatePSO(_cptCreateBlockQueuePSO[0], createBlockQueueCS[0]);
        CreatePSO(_cptCreateBlockQueuePSO[1], createBlockQueueCS[1]);
        CreatePSO(_cptBlockQueueResolvePSO, resolveBlockQueueCS);
#undef  CreatePSO

        _gfxStepInfoPSO.SetRootSignature(_rootsig);
        _gfxStepInfoPSO.SetPrimitiveRestart(
            D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
        _gfxStepInfoPSO.SetInputLayout(
            _countof(inputElementDescs), inputElementDescs);
        _gfxStepInfoPSO.SetDepthStencilState(Graphics::g_DepthStateDisabled);
        _gfxStepInfoPSO.SetSampleMask(UINT_MAX);
        _gfxStepInfoPSO.SetVertexShader(
            stepInfoVS->GetBufferPointer(), stepInfoVS->GetBufferSize());
        _gfxStepInfoDebugPSO = _gfxStepInfoPSO;
        _gfxStepInfoDebugPSO.SetDepthStencilState(
            Graphics::g_DepthStateReadOnly);
        _gfxStepInfoPSO.SetRasterizerState(Graphics::g_RasterizerTwoSided);
        D3D12_RASTERIZER_DESC rastDesc = Graphics::g_RasterizerTwoSided;
        rastDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
        _gfxStepInfoDebugPSO.SetRasterizerState(rastDesc);
        _gfxStepInfoDebugPSO.SetBlendState(Graphics::g_BlendDisable);
        D3D12_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = false;
        blendDesc.IndependentBlendEnable = false;
        blendDesc.RenderTarget[0].BlendEnable = true;
        blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_MIN;
        blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_MAX;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
        _gfxStepInfoPSO.SetBlendState(blendDesc);
        _gfxStepInfoPSO.SetPrimitiveTopologyType(
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        _gfxStepInfoDebugPSO.SetPrimitiveTopologyType(
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
        _gfxStepInfoDebugPSO.SetRenderTargetFormats(
            1, &ColorFormat, DepthFormat);
        _gfxStepInfoPSO.SetRenderTargetFormats(
            1, &_stepInfoTexFormat, DXGI_FORMAT_UNKNOWN);
        _gfxStepInfoPSO.SetPixelShader(
            stepInfoPS->GetBufferPointer(), stepInfoPS->GetBufferSize());
        _gfxStepInfoDebugPSO.SetPixelShader(
            stepInfoPS->GetBufferPointer(), stepInfoDebugPS->GetBufferSize());
        _gfxStepInfoPSO.Finalize();
        _gfxStepInfoDebugPSO.Finalize();
    }

    void _CreateResource()
    {
        HRESULT hr;
        // Feature support checking
        D3D12_FEATURE_DATA_D3D12_OPTIONS FeatureData = {};
        V(Graphics::g_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
            &FeatureData, sizeof(FeatureData)));
        if (SUCCEEDED(hr)) {
            if (FeatureData.TypedUAVLoadAdditionalFormats) {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT FormatSupport =
                {DXGI_FORMAT_R8_SNORM, D3D12_FORMAT_SUPPORT1_NONE,
                    D3D12_FORMAT_SUPPORT2_NONE};
                V(Graphics::g_device->CheckFeatureSupport(
                    D3D12_FEATURE_FORMAT_SUPPORT, &FormatSupport,
                    sizeof(FormatSupport)));
                if (FAILED(hr)) {
                    PRINTERROR("Checking Feature Support Failed");
                }
                if ((FormatSupport.Support2 &
                    D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0) {
                    _typedLoadSupported = true;
                    PRINTINFO("Typed load is supported");
                } else {
                    PRINTWARN("Typed load is not supported");
                }
            } else {
                PRINTWARN("TypedLoad AdditionalFormats load is not supported");
            }
        }

        _CreatePSOs();

        const uint32_t vertexBufferSize = sizeof(cubeVertices);
        _cubeVB.Create(L"Vertex Buffer", ARRAYSIZE(cubeVertices),
            sizeof(XMFLOAT3), (void*)cubeVertices);

        _cubeTriangleStripIB.Create(L"Cube TriangleStrip Index Buffer",
            ARRAYSIZE(cubeTrianglesStripIndices), sizeof(uint16_t),
            (void*)cubeTrianglesStripIndices);

        _cubeLineStripIB.Create(L"Cube LineStrip Index Buffer",
            ARRAYSIZE(cubeLineStripIndices), sizeof(uint16_t),
            (void*)cubeLineStripIndices);
    }
}

TSDFVolume::TSDFVolume()
    : _volBuf(DXGI_FORMAT_R16_FLOAT, XMUINT3(256, 256, 128)),
    _stepInfoTex(XMVectorSet(MAX_DEPTH, 0, 0, 0))
{
    _volParam = &_cbPerCall.vParam;
    _volParam->fVoxelSize = 1.f / 256.f;
    _ratioIdx = 0;
    _cbPerCall.uNumOfBalls = 20;
    _cbPerCall.bBlockRayCast = false;
    _cbPerCall.bInterpolatedNearSurface = false;
    _cbPerCall.bMetaBall = false;
    _cbPerCall.vParam.fSmoothParam = 20.f;
}

TSDFVolume::~TSDFVolume()
{
}

void
TSDFVolume::OnConfiguration()
{
    Core::g_config.FXAA = false;
    Core::g_config.swapChainDesc.BufferCount = 5;
    Core::g_config.swapChainDesc.Width = 1280;
    Core::g_config.swapChainDesc.Height = 800;
}

HRESULT
TSDFVolume::OnCreateResource()
{
    ASSERT(Graphics::g_device);
    // Create resource for volume
    _volBuf.CreateResource();
    _readBackBuffer.Create(L"ReadBackBuffer", 1, sizeof(uint32_t));

    // Initial value for dispatch indirect args. args are thread group count
    // x, y, z. Since we only need 1 dispatch thread group, so we pre-populate
    // 1, 1 for threadgroup Y and Z
    __declspec(align(16)) const uint32_t initArgs[3] = { 0,1,1 };
    _indirectParams.Create(L"TSDFVolume Indirect Params",
        1, sizeof(D3D12_DISPATCH_ARGUMENTS), initArgs);

    const uint3 reso = _volBuf.GetReso();
    _submittedReso = reso;
    _UpdateVolumeSettings(reso);
    _ratioIdx = (uint)(max((int32_t)_ratios.size() - 2, 0));
    _UpdateBlockSettings(_ratios[_ratioIdx]);

    // Create Spacial Structure Buffer
    _CreateBrickVolume(reso, _ratios[_ratioIdx]);

    for (int i = 0; i < MAX_BALLS; ++i) {
        _AddBall();
    }

    std::call_once(_psoCompiled_flag, _CreateResource);

    OnSizeChanged();
    _ResetCameraView();
    return S_OK;
}

void
TSDFVolume::OnDestroy()
{
    _readBackBuffer.Destroy();
    _indirectParams.Destroy();
    _blockWorkBuf.Destroy();
    _volBuf.Destroy();
    _flagVol.Destroy();
    _stepInfoTex.Destroy();
    _cubeVB.Destroy();
    _cubeTriangleStripIB.Destroy();
    _cubeLineStripIB.Destroy();
}

HRESULT
TSDFVolume::OnSizeChanged()
{
    uint32_t width = Core::g_config.swapChainDesc.Width;
    uint32_t height = Core::g_config.swapChainDesc.Height;

    float fAspectRatio = width / (FLOAT)height;
    _cbPerFrame.fWideHeightRatio = fAspectRatio;
    _cbPerFrame.fClipDist = 0.1f;
    float fHFov = XM_PIDIV4;
    _cbPerFrame.fTanHFov = tan(fHFov / 2.f);
    m_camera.Projection(fHFov, fAspectRatio);

    _stepInfoTex.Destroy();
    // Create MinMax Buffer
    _stepInfoTex.Create(L"StepInfoTex", width, height, 0, _stepInfoTexFormat);
    return S_OK;
}

void
TSDFVolume::OnRender(CommandContext& EngineContext)
{
    XMMATRIX view = m_camera.View();
    XMMATRIX proj = m_camera.Projection();

    XMFLOAT4 eyePos;
    XMStoreFloat4(&eyePos, m_camera.Eye());
    _OnIntegrate(EngineContext);
    _OnRender(EngineContext, XMMatrixMultiply(view, proj), view, eyePos);
}

void
TSDFVolume::OnUpdate()
{
    m_camera.ProcessInertia();
    ManagedBuf::BufInterface newBufInterface = _volBuf.GetResource();
    _needVolumeRebuild =
        _curBufInterface.resource[0] != newBufInterface.resource[0];
    _curBufInterface = newBufInterface;

    const uint3& reso = _volBuf.GetReso();
    if (_IsResolutionChanged(reso, _curReso)) {
        _curReso = reso;
        _UpdateVolumeSettings(reso);
        _CreateBrickVolume(reso, _ratios[_ratioIdx]);
    }
    _RenderGui();
}

bool TSDFVolume::OnEvent(MSG* msg)
{
    switch (msg->message) {
    case WM_MOUSEWHEEL: {
        auto delta = GET_WHEEL_DELTA_WPARAM(msg->wParam);
        m_camera.ZoomRadius(-0.002f*delta);
        return true;
    }
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP: {
        auto pointerId = GET_POINTERID_WPARAM(msg->wParam);
        POINTER_INFO pointerInfo;
        if (GetPointerInfo(pointerId, &pointerInfo)) {
            if (msg->message == WM_POINTERDOWN) {
                // Compute pointer position in render units
                POINT p = pointerInfo.ptPixelLocation;
                ScreenToClient(Core::g_hwnd, &p);
                RECT clientRect;
                GetClientRect(Core::g_hwnd, &clientRect);
                p.x = p.x * Core::g_config.swapChainDesc.Width /
                    (clientRect.right - clientRect.left);
                p.y = p.y * Core::g_config.swapChainDesc.Height /
                    (clientRect.bottom - clientRect.top);
                // Camera manipulation
                m_camera.AddPointer(pointerId);
            }
        }
        // Otherwise send it to the camera controls
        m_camera.ProcessPointerFrames(pointerId, &pointerInfo);
        if (msg->message == WM_POINTERUP) m_camera.RemovePointer(pointerId);
        return true;
    }
    }
    return false;
}

void
TSDFVolume::_OnIntegrate(CommandContext& cmdCtx)
{
    ComputeContext& cptCtx = cmdCtx.GetComputeContext();
    if (_resetVolume) {
        _CleanTSDFBuffer(cptCtx, _curBufInterface);
        _CleanBrickVolume(cptCtx);
        _resetVolume = false;
        _needVolumeRebuild = true;
    }

    if (_isAnimated || _needVolumeRebuild) {
        cptCtx.SetRootSignature(_rootsig);
        cptCtx.SetDynamicConstantBufferView(
            0, sizeof(_cbPerFrame), (void*)&_cbPerFrame);
        cptCtx.SetDynamicConstantBufferView(
            1, sizeof(_cbPerCall), (void*)&_cbPerCall);
        if (_useStepInfoTex) {
            cptCtx.TransitionResource(_flagVol,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            _CleanBrickVolume(cptCtx);
            cptCtx.TransitionResource(_flagVol,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        cmdCtx.TransitionResource(*_curBufInterface.resource[0],
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdCtx.TransitionResource(*_curBufInterface.resource[1],
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        _UpdateVolume(cmdCtx, _curBufInterface);
        cmdCtx.BeginResourceTransition(*_curBufInterface.resource[0],
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (_useStepInfoTex) {
            cmdCtx.BeginResourceTransition(_flagVol,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        _needVolumeRebuild = false;
    }
}

void
TSDFVolume::_OnRender(CommandContext& cmdCtx, const DirectX::XMMATRIX& wvp,
    const DirectX::XMMATRIX& mView, const DirectX::XMFLOAT4& eyePos)
{
    _UpdatePerFrameData(wvp, mView, eyePos);

    GraphicsContext& gfxCtx = cmdCtx.GetGraphicsContext();
    gfxCtx.TransitionResource(Graphics::g_SceneColorBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfxCtx.TransitionResource(Graphics::g_SceneDepthBuffer,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, !_useStepInfoTex);

    if (_useStepInfoTex) {
        gfxCtx.TransitionResource(_stepInfoTex,
            D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        gfxCtx.ClearColor(_stepInfoTex);
    }

    gfxCtx.ClearColor(Graphics::g_SceneColorBuffer);
    gfxCtx.ClearDepth(Graphics::g_SceneDepthBuffer);

    gfxCtx.SetRootSignature(_rootsig);
    gfxCtx.SetDynamicConstantBufferView(
        0, sizeof(_cbPerFrame), (void*)&_cbPerFrame);
    gfxCtx.SetDynamicConstantBufferView(
        1, sizeof(_cbPerCall), (void*)&_cbPerCall);
    gfxCtx.SetViewport(Graphics::g_DisplayPlaneViewPort);
    gfxCtx.SetScisor(Graphics::g_DisplayPlaneScissorRect);
    gfxCtx.SetVertexBuffer(0, _cubeVB.VertexBufferView());

    if (_useStepInfoTex) {
        gfxCtx.TransitionResource(_flagVol,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        _RenderNearFar(gfxCtx);
        gfxCtx.TransitionResource(_stepInfoTex,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    gfxCtx.TransitionResource(*_curBufInterface.resource[0],
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    _RenderVolume(gfxCtx, _curBufInterface);
    gfxCtx.BeginResourceTransition(*_curBufInterface.resource[0],
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (_useStepInfoTex) {
        if (_stepInfoDebug) {
            _RenderBrickGrid(gfxCtx);
        }
        cmdCtx.BeginResourceTransition(_stepInfoTex,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
}

void
TSDFVolume::_RenderGui()
{
    ImGui::Begin("TSDFVolume");
    ImGui::Checkbox("Quad Raycast", &_useQuadRaycast);
    ImGui::Checkbox("Animation", &_isAnimated);
    if (_isAnimated) {
        ImGui::Indent();
        ImGui::Checkbox("Block Volume Update", &_blockVolumeUpdate);
        if (_blockVolumeUpdate) {
            ImGui::Indent();
            ImGui::Checkbox("ReadBack Block Count", &_readBack);
            if (_readBack) {
                float usedBlockPct = _blockQueueCounter / (float)_maxJobCount;
                char buf[64];
                sprintf_s(buf, 64, "%d/%d Blocks",
                    _blockQueueCounter, _maxJobCount);
                ImGui::ProgressBar(usedBlockPct, ImVec2(-1.f, 0.f), buf);
            }
            ImGui::Unindent();
        }
        ImGui::Unindent();
    }
    ImGui::Checkbox("Blend Sphere", (bool*)&_cbPerCall.bMetaBall);
    if (_cbPerCall.bMetaBall) {
        ImGui::Indent();
        ImGui::SliderFloat("Blend Param",
            &_volParam->fSmoothParam, 15.f, 40.f);
        ImGui::Unindent();
    }
    if (ImGui::Checkbox("StepInfoTex", &_useStepInfoTex) &&
        _useStepInfoTex) {
        _needVolumeRebuild |= true;
    }
    if (_useStepInfoTex) {
        ImGui::Indent();
        ImGui::Checkbox("Block Ray Cast", (bool*)&_cbPerCall.bBlockRayCast);
        ImGui::Checkbox("Draw Debug Grid", &_stepInfoDebug);
        ImGui::Unindent();
    }
    ImGui::Separator();
    static int iFilterType = (int)_filterType;
    ImGui::Text("Sample Method:");
    ImGui::Checkbox("Interpolate only near surface",
        (bool*)&_cbPerCall.bInterpolatedNearSurface);
    ImGui::RadioButton("Uninterpolated", &iFilterType, kNoFilter);
    ImGui::RadioButton("Trilinear", &iFilterType, kLinearFilter);
    ImGui::RadioButton("Trilinear Sampler", &iFilterType, kSamplerLinear);
    if (_volBuf.GetType() != ManagedBuf::k3DTexBuffer &&
        iFilterType == kSamplerLinear) {
        iFilterType = _filterType;
    }
    _filterType = (FilterType)iFilterType;

    ImGui::Separator();
    ImGui::Text("Buffer Settings:");
    static int uBufferBitChoice = _volBuf.GetBit();
    static int uBufferTypeChoice = _volBuf.GetType();
    ImGui::RadioButton("8Bit", &uBufferBitChoice,
        ManagedBuf::k8Bit); ImGui::SameLine();
    ImGui::RadioButton("16Bit", &uBufferBitChoice,
        ManagedBuf::k16Bit); ImGui::SameLine();
    ImGui::RadioButton("32Bit", &uBufferBitChoice,
        ManagedBuf::k32Bit);
    ImGui::RadioButton("Use Typed Buffer", &uBufferTypeChoice,
        ManagedBuf::kTypedBuffer);
    ImGui::RadioButton("Use Texture3D Buffer", &uBufferTypeChoice,
        ManagedBuf::k3DTexBuffer);
    if ((iFilterType > kLinearFilter &&
        uBufferTypeChoice != ManagedBuf::k3DTexBuffer) ||
        (uBufferTypeChoice == ManagedBuf::kTypedBuffer &&
            _volBuf.GetReso().z > 320)) {
        uBufferTypeChoice = _volBuf.GetType();
    }
    if ((uBufferTypeChoice != _volBuf.GetType() ||
        uBufferBitChoice != _volBuf.GetBit()) &&
        !_volBuf.ChangeResource(_volBuf.GetReso(),
        (ManagedBuf::Type)uBufferTypeChoice,
            (ManagedBuf::Bit)uBufferBitChoice)) {
        uBufferTypeChoice = _volBuf.GetType();
    }

    ImGui::Separator();
    ImGui::Text("Spacial Structure:");
    static int iIdx = _ratioIdx;
    ImGui::SliderInt("BrickBox Ratio", &iIdx, 0,
        (uint)(_ratios.size() - 1), "");
    iIdx = iIdx >= (int)_ratios.size() ? (int)_ratios.size() - 1 : iIdx;
    if (iIdx != _ratioIdx) {
        _ratioIdx = iIdx;
        PRINTINFO("Ratio:%d", _ratios[_ratioIdx]);
        _UpdateBlockSettings(_ratios[_ratioIdx]);
        _CreateBrickVolume(_curReso, _ratios[_ratioIdx]);
        _needVolumeRebuild |= true;
    }
    ImGui::Separator();

    ImGui::Text("Volume Size Settings:");
    static uint3 uiReso = _volBuf.GetReso();
    ImGui::AlignFirstTextHeightToWidgets();
    ImGui::RadioButton("128x256x256##X", (int*)&uiReso.z, 128);
    ImGui::RadioButton("192x384x384##X", (int*)&uiReso.z, 192);
    ImGui::RadioButton("256x512x512##X", (int*)&uiReso.z, 256);
    ImGui::RadioButton("320x640x640##X", (int*)&uiReso.z, 320);
    // Since Buffer Element Count is limited to 2^27, the following reso
    // will not satisfied buffer requirement
    if (uBufferTypeChoice == ManagedBuf::k3DTexBuffer) {
        ImGui::RadioButton("384x768x768##X", (int*)&uiReso.z, 384);
        ImGui::RadioButton("512x1024x1024##X", (int*)&uiReso.z, 512);
    }
    uiReso.x = uiReso.y = uiReso.z * 2;
    if ((_IsResolutionChanged(uiReso, _submittedReso) ||
        _volBuf.GetType() != uBufferTypeChoice) &&
        _volBuf.ChangeResource(
            uiReso, _volBuf.GetType(), ManagedBuf::k32Bit)) {
        PRINTINFO("Reso:%dx%dx%d", uiReso.x, uiReso.y, uiReso.z);
        _submittedReso = uiReso;
    } else {
        uiReso = _submittedReso;
    }

    ImGui::Separator();
    if (ImGui::Button("Recompile All Shaders")) {
        _CreatePSOs();
    }
    ImGui::End();
}

void
TSDFVolume::_CreateBrickVolume(const uint3& reso, const uint ratio)
{
    Graphics::g_cmdListMngr.IdleGPU();
    _flagVol.Destroy();
    _flagVol.Create(L"FlagVol", reso.x / ratio, reso.y / ratio,
        reso.z / ratio, 1, DXGI_FORMAT_R8_UINT);
    _blockWorkBuf.Destroy();
    _maxJobCount =
        (uint32_t)(reso.x * reso.y * reso.z / ratio / ratio / ratio);
    _blockWorkBuf.Create(L"Work Queue", _maxJobCount, 4);
}

void
TSDFVolume::_UpdatePerFrameData(const DirectX::XMMATRIX& wvp,
    const DirectX::XMMATRIX& mView, const DirectX::XMFLOAT4& eyePos)
{
    _cbPerFrame.mWorldViewProj = wvp;
    _cbPerFrame.mView = mView;
    _cbPerFrame.mInvView = XMMatrixInverse(nullptr, mView);
    _cbPerFrame.f4ViewPos = eyePos; 
    if (_isAnimated || _needVolumeRebuild) {
        _animateTime += Core::g_deltaTime;
        for (uint i = 0; i < _cbPerCall.uNumOfBalls; i++) {
            Ball ball = _ballsData[i];
            _cbPerFrame.f4Balls[i].x =
                ball.fOribtRadius * (float)cosf((float)_animateTime *
                    ball.fOribtSpeed + ball.fOribtStartPhase);
            _cbPerFrame.f4Balls[i].y =
                ball.fOribtRadius * (float)sinf((float)_animateTime *
                    ball.fOribtSpeed + ball.fOribtStartPhase);
            _cbPerFrame.f4Balls[i].z =
                0.3f * ball.fOribtRadius * (float)sinf(
                    2.f * (float)_animateTime * ball.fOribtSpeed +
                    ball.fOribtStartPhase);
            _cbPerFrame.f4Balls[i].w = ball.fPower * 30.f;
            _cbPerFrame.f4BallsCol[i] = ball.f4Color;
        }
    }
}

void
TSDFVolume::_UpdateVolumeSettings(const uint3 reso)
{
    uint3& xyz = _volParam->u3VoxelReso;
    if (xyz.x == reso.x && xyz.y == reso.y && xyz.z == reso.z) {
        return;
    }
    xyz = reso;
    const float voxelSize = 0.5f / reso.z;
    _volParam->fVoxelSize = voxelSize;
    _volParam->fTruncDist = 1.75f * voxelSize;
    _volParam->fInvVoxelSize = 1.f / voxelSize;
    _volParam->i3ResoVector = int3(1, reso.x, reso.x * reso.y);
    _volParam->f3HalfVoxelReso =
        float3(reso.x / 2.f, reso.y / 2.f, reso.z / 2.f);
    _volParam->f3HalfVolSize = float3(0.5f * reso.x * voxelSize,
        0.5f * reso.y * voxelSize, 0.5f * reso.z * voxelSize);
    uint3 u3BReso = uint3(reso.x - 2, reso.y - 2, reso.z - 2);
    _volParam->f3BoxMax = float3(0.5f * u3BReso.x * voxelSize,
        0.5f * u3BReso.y * voxelSize, 0.5f * u3BReso.z * voxelSize);
    _volParam->f3BoxMin = float3(-0.5f * u3BReso.x * voxelSize,
        -0.5f * u3BReso.y * voxelSize, -0.5f * u3BReso.z * voxelSize);
    _BuildBrickRatioVector(min(reso.x, min(reso.y, reso.z)), _ratios);
    _ratioIdx = _ratioIdx >= (int)_ratios.size()
        ? (int)_ratios.size() - 1 : _ratioIdx;
    _UpdateBlockSettings(_ratios[_ratioIdx]);
}

void
TSDFVolume::_UpdateBlockSettings(const uint blockVoxelRatio)
{
    _volParam->uVoxelBlockRatio = blockVoxelRatio;
    _volParam->fBlockSize = blockVoxelRatio * _volParam->fVoxelSize;
    uint TGBlockRatio = blockVoxelRatio / THREAD_X;
    _volParam->uThreadGroupBlockRatio = TGBlockRatio;
    _volParam->uThreadGroupPerBlock =
        TGBlockRatio * TGBlockRatio * TGBlockRatio;
}

void
TSDFVolume::_CleanTSDFBuffer(
    ComputeContext& cptCtx, const ManagedBuf::BufInterface& buf)
{
    GPU_PROFILE(cptCtx, L"TSDF Buffer Reset");
    cptCtx.SetPipelineState(_cptTSDFBufResetPSO[buf.type]);
    cptCtx.SetRootSignature(_rootsig);
    cptCtx.SetDynamicDescriptors(2, 0, 2, buf.UAV);
    const uint3 xyz = _volParam->u3VoxelReso;
    cptCtx.Dispatch3D(xyz.x, xyz.y, xyz.z, THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_CleanBrickVolume(ComputeContext& cptCtx)
{
    GPU_PROFILE(cptCtx, L"Volume Reset");
    cptCtx.SetPipelineState(_cptFlagVolResetPSO);
    cptCtx.SetDynamicDescriptors(2, 1, 1, &_flagVol.GetUAV());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelBlockRatio;
    cptCtx.Dispatch3D(xyz.x / ratio, xyz.y / ratio, xyz.z / ratio,
        THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_UpdateVolume(CommandContext& cmdCtx,
    const ManagedBuf::BufInterface& buf)
{
    VolumeStruct type = _useStepInfoTex ? kFlagVol : kVoxel;
    const uint3 xyz = _volParam->u3VoxelReso;
    ComputeContext& cptCtx = cmdCtx.GetComputeContext();
    cptCtx.SetRootSignature(_rootsig);
    if (_blockVolumeUpdate) {
        // Populate BlockQueue with active block
        {
            GPU_PROFILE(cptCtx, L"Block Queue Creation");
            cptCtx.ResetCounter(_blockWorkBuf);
            cptCtx.TransitionResource(_blockWorkBuf,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cptCtx.SetPipelineState(
                _cptCreateBlockQueuePSO[_cbPerCall.bMetaBall]);
            cptCtx.SetDynamicDescriptors(2, 0, 1, &_blockWorkBuf.GetUAV());
            const uint ratio = _volParam->uVoxelBlockRatio;
            cptCtx.Dispatch3D(xyz.x / ratio, xyz.y / ratio, xyz.z / ratio,
                THREAD_X, THREAD_Y, THREAD_Z);
        }
        // Resolve BlockQueue to update indirect args
        {
            GPU_PROFILE(cptCtx, L"Resolve Block Queue");
            cptCtx.SetPipelineState(_cptBlockQueueResolvePSO);
            cptCtx.TransitionResource(_indirectParams,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cptCtx.TransitionResource(_blockWorkBuf,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cptCtx.SetDynamicDescriptors(3, 0, 1,
                &_blockWorkBuf.GetCounterSRV(cptCtx));
            cptCtx.SetDynamicDescriptors(2, 0, 1, &_indirectParams.GetUAV());
            cptCtx.Dispatch(1, 1, 1);
        }
        // Update Block Volumes
        {
            GPU_PROFILE(cptCtx, L"Volume Block Update");
            cptCtx.SetPipelineState(_cptBlockVolumeUpdatePSO
                [buf.type][type][_cbPerCall.bMetaBall]);
            cptCtx.SetDynamicDescriptors(3, 0, 1, &_blockWorkBuf.GetSRV());
            cptCtx.SetDynamicDescriptors(2, 0, 2, buf.UAV);
            cptCtx.SetDynamicDescriptors(2, 2, 1, &_flagVol.GetUAV());
            cptCtx.TransitionResource(_indirectParams,
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            cptCtx.DispatchIndirect(_indirectParams, 0);
        }

        // Copy data to read back buffer and read it back
        if (_readBack) {
            Graphics::g_cmdListMngr.WaitForFence(_readBackFence);
            _readBackBuffer.Map(nullptr,
                reinterpret_cast<void**>(&_readBackPtr));
            _blockQueueCounter = *_readBackPtr;
            _readBackBuffer.Unmap(nullptr);
            cptCtx.CopyBufferRegion(
                _readBackBuffer, 0, _blockWorkBuf.GetCounterBuffer(), 0, 4);
            _readBackFence = cptCtx.Flush();
        }
    } else {
        GPU_PROFILE(cptCtx, L"Volume Updating");
        cptCtx.TransitionResource(_indirectParams,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        cptCtx.SetPipelineState(
            _cptUpdatePSO[buf.type][type][_cbPerCall.bMetaBall]);
        cptCtx.SetDynamicDescriptors(2, 0, 2, buf.UAV);
        cptCtx.SetDynamicDescriptors(2, 2, 1, &_flagVol.GetUAV());
        if (!_typedLoadSupported) {
            cptCtx.SetDynamicDescriptors(3, 1, 2, buf.SRV);
        }
        cptCtx.Dispatch3D(xyz.x, xyz.y, xyz.z, THREAD_X, THREAD_Y, THREAD_Z);
    }
}

void
TSDFVolume::_RenderNearFar(GraphicsContext& gfxCtx)
{
    GPU_PROFILE(gfxCtx, L"Render NearFar");
    gfxCtx.SetPipelineState(_gfxStepInfoPSO);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    gfxCtx.SetRenderTargets(1, &_stepInfoTex.GetRTV());
    gfxCtx.SetDynamicDescriptors(3, 1, 1, &_flagVol.GetSRV());
    gfxCtx.SetIndexBuffer(_cubeTriangleStripIB.IndexBufferView());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelBlockRatio;
    uint BrickCount = xyz.x * xyz.y * xyz.z / ratio / ratio / ratio;
    gfxCtx.DrawIndexedInstanced(CUBE_TRIANGLESTRIP_LENGTH, BrickCount, 0, 0, 0);
}

void
TSDFVolume::_RenderVolume(GraphicsContext& gfxCtx,
    const ManagedBuf::BufInterface& buf)
{
    GPU_PROFILE(gfxCtx, L"Rendering");
    VolumeStruct type = _useStepInfoTex ? kFlagVol : kVoxel;
    gfxCtx.SetPipelineState(
        _gfxRenderPSO[buf.type][type][_filterType][_useQuadRaycast]);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    gfxCtx.SetDynamicDescriptors(3, 0, 1, buf.SRV);
    if (_useStepInfoTex) {
        gfxCtx.SetDynamicDescriptors(3, 1, 1, &_stepInfoTex.GetSRV());
        gfxCtx.SetDynamicDescriptors(3, 2, 1, &_flagVol.GetSRV());
    }
    gfxCtx.SetRenderTargets(1, &Graphics::g_SceneColorBuffer.GetRTV(),
        Graphics::g_SceneDepthBuffer.GetDSV());
    if (_useQuadRaycast) {
        gfxCtx.Draw(3);
    } else {
        gfxCtx.SetIndexBuffer(_cubeTriangleStripIB.IndexBufferView());
        gfxCtx.DrawIndexed(CUBE_TRIANGLESTRIP_LENGTH);
    }
}

void
TSDFVolume::_RenderBrickGrid(GraphicsContext& gfxCtx)
{
    GPU_PROFILE(gfxCtx, L"Render BrickGrid");
    gfxCtx.SetPipelineState(_gfxStepInfoDebugPSO);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
    gfxCtx.SetRenderTargets(1, &Graphics::g_SceneColorBuffer.GetRTV(),
        Graphics::g_SceneDepthBuffer.GetDSV());
    gfxCtx.SetDynamicDescriptors(3, 1, 1, &_flagVol.GetSRV());
    gfxCtx.SetIndexBuffer(_cubeLineStripIB.IndexBufferView());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelBlockRatio;
    uint BrickCount = xyz.x * xyz.y * xyz.z / ratio / ratio / ratio;
    gfxCtx.DrawIndexedInstanced(CUBE_LINESTRIP_LENGTH, BrickCount, 0, 0, 0);
}

void
TSDFVolume::_ResetCameraView()
{
    auto center = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
    auto radius = m_camOrbitRadius;
    auto maxRadius = m_camMaxOribtRadius;
    auto minRadius = m_camMinOribtRadius;
    auto longAngle = 3.1415926f / 2.f;//4.50f;
    auto latAngle = 3.1415926f / 2.f;//1.45f;
    m_camera.View(center, radius, minRadius, maxRadius, longAngle, latAngle);
}

void
TSDFVolume::_AddBall()
{
    Ball ball;
    float r = (0.6f * frand() + 0.7f) * _cbPerCall.vParam.u3VoxelReso.x *
        _cbPerCall.vParam.fVoxelSize * 0.05f;
    ball.fPower = r * r;
    ball.fOribtRadius = _cbPerCall.vParam.u3VoxelReso.x *
        _cbPerCall.vParam.fVoxelSize * (0.3f + (frand() - 0.3f) * 0.2f);

    if (ball.fOribtRadius + r > 0.45f * _cbPerCall.vParam.u3VoxelReso.x *
        _cbPerCall.vParam.fVoxelSize) {
        r = 0.45f * _cbPerCall.vParam.u3VoxelReso.x *
            _cbPerCall.vParam.fVoxelSize - ball.fOribtRadius;
        ball.fPower = r * r;
    }
    float speedF = 6.f * (frand() - 0.5f);
    if (abs(speedF) < 1.f) {
        speedF = (speedF > 0.f ? 1.f : -1.f) * 1.f;
    }
    ball.fOribtSpeed = 1.0f / ball.fPower * 0.0005f * speedF;
    ball.fOribtStartPhase = frand() * 6.28f;

    float alpha = frand() * 6.28f;
    float beta = frand() * 6.28f;
    float gamma = frand() * 6.28f;

    XMFLOAT3 xPositive = XMFLOAT3(1.f, 0.f, 0.f);
    XMMATRIX rMatrix = XMMatrixRotationRollPitchYaw(alpha, beta, gamma);
    XMVECTOR colVect = XMVector3TransformNormal(
        XMLoadFloat3(&xPositive), rMatrix);
    XMFLOAT4 col;
    XMStoreFloat4(&col, colVect);
    col.x = abs(col.x);
    col.y = abs(col.y);
    col.z = abs(col.z);
    col.w = 1.f;
    ball.f4Color = col;

    if (_ballsData.size() < MAX_BALLS) {
        _ballsData.push_back(ball);
    }
}