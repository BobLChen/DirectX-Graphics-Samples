//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include "Denoiser.h"
#include "GameInput.h"
#include "EngineTuning.h"
#include "EngineProfiling.h"
#include "GpuTimeManager.h"
#include "Denoiser.h"
#include "D3D12RaytracingAmbientOcclusion.h"
#include "Composition.h"

using namespace std;
using namespace DX;
using namespace DirectX;
using namespace SceneEnums;


namespace Denoiser_Args
{
    // Temporal Cache.
    BoolVar TemporalSupersampling_Enabled(L"Render/AO/RTAO/Temporal Cache/Enabled", true);
    BoolVar TemporalSupersampling_CacheRawAOValue(L"Render/AO/RTAO/Temporal Cache/Cache Raw AO Value", true);
    NumVar TemporalSupersampling_MinSmoothingFactor(L"Render/AO/RTAO/Temporal Cache/Min Smoothing Factor", 0.03f, 0, 1.f, 0.01f);
    NumVar TemporalSupersampling_DepthTolerance(L"Render/AO/RTAO/Temporal Cache/Depth tolerance [%%]", 0.05f, 0, 1.f, 0.001f);
    BoolVar TemporalSupersampling_UseWorldSpaceDistance(L"Render/AO/RTAO/Temporal Cache/Use world space distance", false);
    BoolVar TemporalSupersampling_PerspectiveCorrectDepthInterpolation(L"Render/AO/RTAO/Temporal Cache/Depth testing/Use perspective correct depth interpolation", false);
    BoolVar TemporalSupersampling_UseDepthWeights(L"Render/AO/RTAO/Temporal Cache/Use depth weights", true);
    BoolVar TemporalSupersampling_UseNormalWeights(L"Render/AO/RTAO/Temporal Cache/Use normal weights", true);
    BoolVar TemporalSupersampling_ForceUseMinSmoothingFactor(L"Render/AO/RTAO/Temporal Cache/Force min smoothing factor", false);
    BoolVar TemporalSupersampling_CacheDenoisedOutput(L"Render/AO/RTAO/Temporal Cache/Cache denoised output", true);
    IntVar TemporalSupersampling_CacheDenoisedOutputPassNumber(L"Render/AO/RTAO/Temporal Cache/Cache denoised output - pass number", 0, 0, 10, 1);
    BoolVar TemporalSupersampling_ClampCachedValues_UseClamping(L"Render/AO/RTAO/Temporal Cache/Clamping/Enabled", true);
    BoolVar TemporalSupersampling_CacheSquaredMean(L"Render/AO/RTAO/Temporal Cache/Cached SquaredMean", false);
    NumVar TemporalSupersampling_ClampCachedValues_StdDevGamma(L"Render/AO/RTAO/Temporal Cache/Clamping/Std.dev gamma", 1.0f, 0.1f, 20.f, 0.1f);
    NumVar TemporalSupersampling_ClampCachedValues_MinStdDevTolerance(L"Render/AO/RTAO/Temporal Cache/Clamping/Minimum std.dev", 0.04f, 0.0f, 1.f, 0.01f);
    NumVar TemporalSupersampling_ClampDifferenceToTsppScale(L"Render/AO/RTAO/Temporal Cache/Clamping/Tspp scale", 4.00f, 0, 10.f, 0.05f);
    NumVar TemporalSupersampling_ClampCachedValues_AbsoluteDepthTolerance(L"Render/AO/RTAO/Temporal Cache/Depth threshold/Absolute depth tolerance", 1.0f, 0.0f, 100.f, 1.f);
    NumVar TemporalSupersampling_ClampCachedValues_DepthBasedDepthTolerance(L"Render/AO/RTAO/Temporal Cache/Depth threshold/Depth based depth tolerance", 1.0f, 0.0f, 100.f, 1.f);
    NumVar TemporalSupersampling_ClampCachedValues_DepthSigma(L"Render/AO/RTAO/Temporal Cache/Depth threshold/Depth sigma", 1.0f, 0.0f, 10.f, 0.01f);

    IntVar VarianceBilateralFilterKernelWidth(L"Render/RTAO/GpuKernels/CalculateVariance/Kernel width", 9, 3, 9, 2);

    // ToDoF test perf impact / visual quality gain at the end. Document.
    BoolVar PerspectiveCorrectDepthInterpolation(L"Render/AO/RTAO/Denoising/Pespective Correct Depth Interpolation", true);

    BoolVar UseAdaptiveKernelSize(L"Render/AO/RTAO/Denoising/AdaptiveKernelSize/Enabled", true);
    BoolVar KernelRadius_RotateKernel_Enabled(L"Render/AO/RTAO/Denoising/AdaptiveKernelSize/Rotate kernel radius/Enabled", true);
    IntVar KernelRadius_RotateKernel_NumCycles(L"Render/AO/RTAO/AdaptiveKernelSize/Rotate kernel radius/Num cycles", 3, 0, 10, 1);
    IntVar FilterMinKernelWidth(L"Render/AO/RTAO/Denoising/AdaptiveKernelSize/Min kernel width", 3, 3, 101);
    NumVar FilterMaxKernelWidthPercentage(L"Render/AO/RTAO/Denoising/AdaptiveKernelSize/Max kernel width [%% of screen width]", 1.5f, 0, 100, 0.1f);
    NumVar FilterVarianceSigmaScaleOnSmallKernels(L"Render/AO/RTAO/Denoising/AdaptiveKernelSize/Variance sigma scale on small kernels", 2.0f, 1.0f, 20.f, 0.5f);
    NumVar AdaptiveKernelSize_RayHitDistanceScaleFactor(L"Render/AO/RTAO/Denoising/AdaptiveKernelSize/Hit distance scale factor", 0.02f, 0.001f, 0.1f, 0.001f);
    NumVar AdaptiveKernelSize_RayHitDistanceScaleExponent(L"Render/AO/RTAO/Denoising/AdaptiveKernelSize/Hit distance scale exponent", 2.0f, 1.0f, 5.0f, 0.1f);
    BoolVar Variance_UseDepthWeights(L"Render/AO/RTAO/Denoising/Variance/Use normal weights", true);
    BoolVar Variance_UseNormalWeights(L"Render/AO/RTAO/Denoising/Variance/Use normal weights", true);
    BoolVar ForceDenoisePass(L"Render/AO/RTAO/Denoising/Force denoise pass", false);
    IntVar MinTsppToUseTemporalVariance(L"Render/AO/RTAO/Denoising/Min Temporal Variance Tspp", 4, 1, 40);
    NumVar MinVarianceToDenoise(L"Render/AO/RTAO/Denoising/Min Variance to denoise", 0.0f, 0.0f, 1.f, 0.01f);
    // ToDo specify which variance - local or temporal
    BoolVar UseSmoothedVariance(L"Render/AO/RTAO/Denoising/Use smoothed variance", false);

    BoolVar LowerWeightForStaleSamples(L"Render/AO/RTAO/Denoising/Scale down stale samples weight", false);
    BoolVar FilterWeightByTspp(L"Render/AO/RTAO/Denoising/Filter weight by tspp", false);


#define MIN_NUM_PASSES_LOW_TSPP 2 // THe blur writes to the initial input resource and thus must numPasses must be 2+. ToDo Unused
#define MAX_NUM_PASSES_LOW_TSPP 6
    BoolVar LowTspp(L"Render/AO/RTAO/Denoising/Low tspp filter/enabled", true);
    IntVar LowTsppMaxTspp(L"Render/AO/RTAO/Denoising/Low tspp filter/Max tspp", 12, 0, 33);
    IntVar LowTspBlurPasses(L"Render/AO/RTAO/Denoising/Low tspp filter/Num blur passes", 3, 2, MAX_NUM_PASSES_LOW_TSPP);
    BoolVar LowTsppUseUAVReadWrite(L"Render/AO/RTAO/Denoising/Low tspp filter/Use single UAV resource Read+Write", true);
    NumVar LowTsppDecayConstant(L"Render/AO/RTAO/Denoising/Low tspp filter/Decay constant", 1.0f, 0.1f, 32.f, 0.1f);
    BoolVar LowTsppFillMissingValues(L"Render/AO/RTAO/Denoising/Low tspp filter/Post-Temporal fill in missing values", true);
    NumVar LowTsppMinNormalWeight(L"Render/AO/RTAO/Denoising/Low tspp filter/Normal Weights/Min weight", 0.25f, 0.0f, 1.f, 0.05f);
    NumVar LowTsppNormalExponent(L"Render/AO/RTAO/Denoising/Low tspp filter/Normal Weights/Exponent", 4.0f, 1.0f, 32.f, 1.0f);

    const WCHAR* Modes[RTAOGpuKernels::AtrousWaveletTransformCrossBilateralFilter::FilterType::Count] = { L"EdgeStoppingGaussian3x3", L"EdgeStoppingGaussian5x5" };
    EnumVar Mode(L"Render/AO/RTAO/Denoising/Mode", RTAOGpuKernels::AtrousWaveletTransformCrossBilateralFilter::FilterType::EdgeStoppingGaussian3x3, RTAOGpuKernels::AtrousWaveletTransformCrossBilateralFilter::FilterType::Count, Modes);
    IntVar AtrousFilterPasses(L"Render/AO/RTAO/Denoising/Num passes", 1, 1, Denoiser::c_MaxAtrousDesnoisePasses, 1);
    NumVar AODenoiseValueSigma(L"Render/AO/RTAO/Denoising/Value Sigma", 1.0f, 0.0f, 30.0f, 0.1f);

    // ToDo remove
    NumVar WeightScale(L"Render/AO/RTAO/Denoising/Weight Scale", 1, 0.0f, 5.0f, 0.01f);
    NumVar AODenoiseDepthSigma(L"Render/AO/RTAO/Denoising/Depth Sigma", 1.0f, 0.0f, 10.0f, 0.02f); 
    NumVar AODenoiseDepthWeightCutoff(L"Render/AO/RTAO/Denoising/Depth Weight Cutoff", 0.2f, 0.0f, 2.0f, 0.01f);
    NumVar AODenoiseNormalSigma(L"Render/AO/RTAO/Denoising/Normal Sigma", 64, 0, 256, 4);
}


DXGI_FORMAT Denoiser::ResourceFormat(ResourceType resourceType)
{
    switch (resourceType)
    {
    case ResourceType::Variance: return DXGI_FORMAT_R16_FLOAT;
    case ResourceType::LocalMeanVariance: return DXGI_FORMAT_R16G16_FLOAT;
    }

    return DXGI_FORMAT_UNKNOWN;
}

void Denoiser::Setup(shared_ptr<DeviceResources> deviceResources, shared_ptr<DX::DescriptorHeap> descriptorHeap)
{
    m_deviceResources = deviceResources;
    m_cbvSrvUavHeap = descriptorHeap;

    CreateDeviceDependentResources();
}

// Create resources that depend on the device.
void Denoiser::CreateDeviceDependentResources()
{
    CreateAuxilaryDeviceResources();
}

void Denoiser::CreateAuxilaryDeviceResources()
{
    auto device = m_deviceResources->GetD3DDevice();
    m_fillInCheckerboardKernel.Initialize(device, Sample::FrameCount);
    m_gaussianSmoothingKernel.Initialize(device, Sample::FrameCount);
    m_temporalCacheReverseReprojectKernel.Initialize(device, Sample::FrameCount);
    m_temporalCacheBlendWithCurrentFrameKernel.Initialize(device, Sample::FrameCount);
    m_atrousWaveletTransformFilter.Initialize(device, c_MaxAtrousDesnoisePasses, Sample::FrameCount);
    m_calculateMeanVarianceKernel.Initialize(device, Sample::FrameCount, 5 * MaxCalculateVarianceKernelInvocationsPerFrame); // ToDo revise the ount
    m_bilateralFilterKernel.Initialize(device, Sample::FrameCount, MAX_NUM_PASSES_LOW_TSPP);
}


// Run() can be optionally called in two explicit stages. This can
// be beneficial to retrieve temporally reprojected values 
// and configure current frame AO raytracing off of that 
// (such as vary spp based on average ray hit distance or tspp).
// Otherwise, all denoiser steps can be run via a single execute call.
void Denoiser::Run(Scene& scene, Pathtracer& pathtracer, RTAO& rtao, DenoiseStage stage)
{
    auto commandList = m_deviceResources->GetCommandList();
    ScopedTimer _prof(L"Denoise", commandList);

    if (stage & Denoise_Stage1_TemporalSupersamplingReverseReproject)
    {
        TemporalSupersamplingReverseReproject(scene, pathtracer);
    }

    if (stage & Denoise_Stage2_Denoise)
    {
        TemporalSupersamplingBlendWithCurrentFrame(rtao);
        ApplyAtrousWaveletTransformFilter(pathtracer, rtao);

        if (Denoiser_Args::LowTspp)
        {
            BlurDisocclusions(pathtracer);
        }
    }
}

void Denoiser::CreateResolutionDependentResources()
{
    auto device = m_deviceResources->GetD3DDevice();

    m_atrousWaveletTransformFilter.CreateInputResourceSizeDependentResources(device, m_cbvSrvUavHeap.get(), m_denoisingWidth, m_denoisingHeight, RTAO::ResourceFormat(RTAO::ResourceType::AOCoefficient));
    CreateTextureResources();
}


void Denoiser::SetResolution(UINT width, UINT height)
{
    m_denoisingWidth = width;
    m_denoisingHeight = height;

    CreateResolutionDependentResources();
}
    

void Denoiser::CreateTextureResources()
{
    auto device = m_deviceResources->GetD3DDevice();
    D3D12_RESOURCE_STATES initialResourceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Temporal cache resources.
    {
        for (UINT i = 0; i < 2; i++)
        {
            // Preallocate subsequent descriptor indices for both SRV and UAV groups.
            m_temporalCache[i][0].uavDescriptorHeapIndex = m_cbvSrvUavHeap->AllocateDescriptorIndices(TemporalSupersampling::Count);
            m_temporalCache[i][0].srvDescriptorHeapIndex = m_cbvSrvUavHeap->AllocateDescriptorIndices(TemporalSupersampling::Count);
            for (UINT j = 0; j < TemporalSupersampling::Count; j++)
            {
                m_temporalCache[i][j].uavDescriptorHeapIndex = m_temporalCache[i][0].uavDescriptorHeapIndex + j;
                m_temporalCache[i][j].srvDescriptorHeapIndex = m_temporalCache[i][0].srvDescriptorHeapIndex + j;
            }

            // ToDo cleanup raytracing resolution - twice for coefficient.
            CreateRenderTargetResource(device, DXGI_FORMAT_R8_UINT, m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_temporalCache[i][TemporalSupersampling::Tspp], initialResourceState, L"Temporal Cache: Tspp");
            CreateRenderTargetResource(device, RTAO::ResourceFormat(RTAO::ResourceType::AOCoefficient), m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_temporalCache[i][TemporalSupersampling::CoefficientSquaredMean], initialResourceState, L"Temporal Cache: Coefficient Squared Mean");
            CreateRenderTargetResource(device, RTAO::ResourceFormat(RTAO::ResourceType::RayHitDistance), m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_temporalCache[i][TemporalSupersampling::RayHitDistance], initialResourceState, L"Temporal Cache: Ray Hit Distance");
            CreateRenderTargetResource(device, RTAO::ResourceFormat(RTAO::ResourceType::AOCoefficient), m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_temporalAOCoefficient[i], initialResourceState, L"Render/AO Temporally Supersampled Coefficient");
        }
    }

    for (UINT i = 0; i < 2; i++)
    {
        CreateRenderTargetResource(device, RTAO::ResourceFormat(RTAO::ResourceType::AOCoefficient), m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_temporalSupersampling_blendedAOCoefficient[i], initialResourceState, L"Temporal Supersampling: AO coefficient current frame blended with the cache.");
    }
    CreateRenderTargetResource(device, DXGI_FORMAT_R16G16B16A16_UINT, m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_cachedTsppValueSquaredValueRayHitDistance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Temporal Supersampling intermediate reprojected Tspp, Value, Squared Mean Value, Ray Hit Distance");

    // Variance resources
    {
        {
            for (UINT i = 0; i < AOVarianceResource::Count; i++)
            {
                CreateRenderTargetResource(device, ResourceFormat(ResourceType::Variance), m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_varianceResources[i], initialResourceState, L"Post Temporal Reprojection Variance");
                CreateRenderTargetResource(device, ResourceFormat(ResourceType::LocalMeanVariance), m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_localMeanVarianceResources[i], initialResourceState, L"Local Mean Variance");
            }
        }
    }

    // ToDo remove obsolete resources, QuarterResAO event triggers this so we may not need all low/gbuffer width AO resources.
    CreateRenderTargetResource(device, DXGI_FORMAT_R8_UNORM, m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_multiPassDenoisingBlurStrength, initialResourceState, L"Disocclusion Denoising Blur Strength");
    CreateRenderTargetResource(device, COMPACT_NORMAL_DEPTH_DXGI_FORMAT, m_denoisingWidth, m_denoisingHeight, m_cbvSrvUavHeap.get(), &m_prevFrameGBufferNormalDepth, initialResourceState, L"Previous Frame GBuffer Normal Depth");
}


// Retrieves values from previous frame via reverse reprojection.
void Denoiser::TemporalSupersamplingReverseReproject(Scene& scene, Pathtracer& pathtracer)
{
    auto commandList = m_deviceResources->GetCommandList();
    auto resourceStateTracker = m_deviceResources->GetGpuResourceStateTracker();

    ScopedTimer _prof(L"Temporal Supersampling p1 (Reverse Reprojection)", commandList);
        
    // Ping-pong input output indices across frames.
    UINT temporalCachePreviousFrameResourceIndex = m_temporalCacheCurrentFrameResourceIndex;
    m_temporalCacheCurrentFrameResourceIndex = (m_temporalCacheCurrentFrameResourceIndex + 1) % 2;

    UINT temporalCachePreviousFrameTemporalAOCoeficientResourceIndex = m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex;
    m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex = (m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex + 1) % 2;
    
    // ToDo does this comment apply here, move it to the shader?
    // Calculate reverse projection transform T to the previous frame's screen space coordinates.
    //  xy(t-1) = xy(t) * T     // ToDo check mul order
    // The reverse projection transform consists:
    //  1) reverse projecting from current's frame screen space coordinates to world space coordinates
    //  2) projecting from world space coordinates to previous frame's screen space coordinates
    //
    //  T = inverse(P(t)) * inverse(V(t)) * V(t-1) * P(t-1) 
    //      where P is a projection transform and V is a view transform. 
    auto& camera = scene.Camera();
    auto& prevFrameCamera = scene.PrevFrameCamera();
    XMMATRIX view, proj, prevView, prevProj;
    camera.GetProj(&proj, m_denoisingWidth, m_denoisingHeight);
    prevFrameCamera.GetProj(&prevProj, m_denoisingWidth, m_denoisingHeight);

    view = XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSetW(camera.At() - camera.Eye(), 1), camera.Up());
    prevView = XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSetW(prevFrameCamera.At() - prevFrameCamera.Eye(), 1), prevFrameCamera.Up());

    XMMATRIX viewProj = view * proj;
    XMMATRIX prevViewProj = prevView * prevProj;
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);
    XMMATRIX prevInvViewProj = XMMatrixInverse(nullptr, prevViewProj);

    // Transition output resource to UAV state.        
    {
        resourceStateTracker->TransitionResource(&m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::Tspp], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        resourceStateTracker->TransitionResource(&m_cachedTsppValueSquaredValueRayHitDistance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    GpuResource (&GBufferResources)[GBufferResource::Count] = pathtracer.GBufferResources(RTAO_Args::QuarterResAO);

    UINT maxTspp = static_cast<UINT>(1 / Denoiser_Args::TemporalSupersampling_MinSmoothingFactor);
    resourceStateTracker->FlushResourceBarriers();

    m_temporalCacheReverseReprojectKernel.Run(
        commandList,
        m_denoisingWidth,
        m_denoisingHeight,
        m_cbvSrvUavHeap->GetHeap(),
        GBufferResources[GBufferResource::SurfaceNormalDepth].gpuDescriptorReadAccess,
        GBufferResources[GBufferResource::PartialDepthDerivatives].gpuDescriptorReadAccess,
        GBufferResources[GBufferResource::ReprojectedNormalDepth].gpuDescriptorReadAccess,
        GBufferResources[GBufferResource::MotionVector].gpuDescriptorReadAccess,
        m_temporalAOCoefficient[temporalCachePreviousFrameTemporalAOCoeficientResourceIndex].gpuDescriptorReadAccess,
        m_prevFrameGBufferNormalDepth.gpuDescriptorReadAccess,
        m_temporalCache[temporalCachePreviousFrameResourceIndex][TemporalSupersampling::Tspp].gpuDescriptorReadAccess,
        m_temporalCache[temporalCachePreviousFrameResourceIndex][TemporalSupersampling::CoefficientSquaredMean].gpuDescriptorReadAccess,
        m_temporalCache[temporalCachePreviousFrameResourceIndex][TemporalSupersampling::RayHitDistance].gpuDescriptorReadAccess,
        m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::Tspp].gpuDescriptorWriteAccess,
        m_cachedTsppValueSquaredValueRayHitDistance.gpuDescriptorWriteAccess,
        Denoiser_Args::TemporalSupersampling_MinSmoothingFactor,
        Denoiser_Args::TemporalSupersampling_DepthTolerance,
        Denoiser_Args::TemporalSupersampling_UseDepthWeights,
        Denoiser_Args::TemporalSupersampling_UseNormalWeights,
        Denoiser_Args::TemporalSupersampling_ClampCachedValues_AbsoluteDepthTolerance,
        Denoiser_Args::TemporalSupersampling_ClampCachedValues_DepthBasedDepthTolerance,
        Denoiser_Args::TemporalSupersampling_ClampCachedValues_DepthSigma,
        Denoiser_Args::TemporalSupersampling_UseWorldSpaceDistance,
        RTAO_Args::QuarterResAO,
        Denoiser_Args::TemporalSupersampling_PerspectiveCorrectDepthInterpolation,
        Sample::g_debugOutput,
        invViewProj,
        prevInvViewProj,
        maxTspp);

    // Transition output resources to SRV state.
    // All the others are used as input/output UAVs in 2nd stage of Temporal Supersampling.
    {
        resourceStateTracker->TransitionResource(&m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::Tspp], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resourceStateTracker->TransitionResource(&m_cachedTsppValueSquaredValueRayHitDistance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resourceStateTracker->InsertUAVBarrier(&m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::Tspp]);
    }

    // Cache the normal depth resource.
    {
        // TODO: replace copy with using a source resource directly.
        ScopedTimer _prof(L"Cache normal depth resource", commandList);
        CopyTextureRegion(
            commandList,
            GBufferResources[GBufferResource::SurfaceNormalDepth].GetResource(),
            m_prevFrameGBufferNormalDepth.GetResource(),
            &CD3DX12_BOX(0, 0, m_denoisingWidth, m_denoisingHeight),
            GBufferResources[GBufferResource::SurfaceNormalDepth].m_UsageState,
            m_prevFrameGBufferNormalDepth.m_UsageState);
    }
}

// Blends reprojected values with current frame values.
// Inactive pixels are filtered from active neighbors on checkerboard sampling
// before the blend operation.
void Denoiser::TemporalSupersamplingBlendWithCurrentFrame(RTAO& rtao)
{
    auto commandList = m_deviceResources->GetCommandList();
    auto resourceStateTracker = m_deviceResources->GetGpuResourceStateTracker();

    ScopedTimer _prof(L"TemporalSupersamplingBlendWithCurrentFrame", commandList);

    GpuResource* AOResources = rtao.AOResources();

    // Transition all output resources to UAV state.
    {
        resourceStateTracker->TransitionResource(&m_localMeanVarianceResources[AOVarianceResource::Raw], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        resourceStateTracker->InsertUAVBarrier(&AOResources[AOResource::AmbientCoefficient]);
    }

    bool isCheckerboardSamplingEnabled;
    bool checkerboardLoadEvenPixels;
    rtao.GetRayGenParameters(&isCheckerboardSamplingEnabled, &checkerboardLoadEvenPixels);

    // Calculate local mean and variance for clamping during the blend operation.
    {
        ScopedTimer _prof(L"Calculate Mean and Variance", commandList);
        resourceStateTracker->FlushResourceBarriers();
        m_calculateMeanVarianceKernel.Run(
            commandList,
            m_cbvSrvUavHeap->GetHeap(),
            m_denoisingWidth,
            m_denoisingHeight,
            AOResources[AOResource::AmbientCoefficient].gpuDescriptorReadAccess,
            m_localMeanVarianceResources[AOVarianceResource::Raw].gpuDescriptorWriteAccess,
            Denoiser_Args::VarianceBilateralFilterKernelWidth,
            isCheckerboardSamplingEnabled,
            checkerboardLoadEvenPixels);

        // Interpolate the variance for the inactive cells from the valid checherkboard cells.
        if (isCheckerboardSamplingEnabled)
        {
            bool fillEvenPixels = !checkerboardLoadEvenPixels;
            resourceStateTracker->FlushResourceBarriers();
            m_fillInCheckerboardKernel.Run(
                commandList,
                m_cbvSrvUavHeap->GetHeap(),
                m_denoisingWidth,
                m_denoisingHeight,
                m_localMeanVarianceResources[AOVarianceResource::Raw].gpuDescriptorWriteAccess,
                fillEvenPixels);
        }

        resourceStateTracker->TransitionResource(&m_localMeanVarianceResources[AOVarianceResource::Raw], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resourceStateTracker->InsertUAVBarrier(&m_localMeanVarianceResources[AOVarianceResource::Raw]);
    }

    {
        resourceStateTracker->InsertUAVBarrier(&m_localMeanVarianceResources[AOVarianceResource::Smoothed]);
    }


    // ToDo remove?
    bool fillInMissingValues = false;   // ToDo fix up barriers if changing this to true
#if 0
    // ToDo?
    Denoiser_Args::LowTsppFillMissingValues
        && rtao.GetSpp() < 1;
#endif
    GpuResource* TemporalOutCoefficient = fillInMissingValues ? &m_temporalSupersampling_blendedAOCoefficient[0] : &m_temporalAOCoefficient[m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex];

    // Transition output resource to UAV state.      
    {
        resourceStateTracker->TransitionResource(TemporalOutCoefficient, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        resourceStateTracker->TransitionResource(&m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::Tspp], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        resourceStateTracker->TransitionResource(&m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::CoefficientSquaredMean], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        resourceStateTracker->TransitionResource(&m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::RayHitDistance], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        resourceStateTracker->TransitionResource(&m_varianceResources[AOVarianceResource::Raw], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        resourceStateTracker->TransitionResource(&m_multiPassDenoisingBlurStrength, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        resourceStateTracker->InsertUAVBarrier(&m_cachedTsppValueSquaredValueRayHitDistance);
    }

    resourceStateTracker->FlushResourceBarriers();
    m_temporalCacheBlendWithCurrentFrameKernel.Run(
        commandList,
        m_denoisingWidth,
        m_denoisingHeight,
        m_cbvSrvUavHeap->GetHeap(),
        AOResources[AOResource::AmbientCoefficient].gpuDescriptorReadAccess,
        m_localMeanVarianceResources[AOVarianceResource::Raw].gpuDescriptorReadAccess,
        AOResources[AOResource::RayHitDistance].gpuDescriptorReadAccess,
        TemporalOutCoefficient->gpuDescriptorWriteAccess,
        m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::Tspp].gpuDescriptorWriteAccess,
        m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::CoefficientSquaredMean].gpuDescriptorWriteAccess,
        m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::RayHitDistance].gpuDescriptorWriteAccess,
        m_cachedTsppValueSquaredValueRayHitDistance.gpuDescriptorReadAccess,
        m_varianceResources[AOVarianceResource::Raw].gpuDescriptorWriteAccess,
        m_multiPassDenoisingBlurStrength.gpuDescriptorWriteAccess,
        Denoiser_Args::TemporalSupersampling_MinSmoothingFactor,
        Denoiser_Args::TemporalSupersampling_ForceUseMinSmoothingFactor,
        Denoiser_Args::TemporalSupersampling_ClampCachedValues_UseClamping,
        Denoiser_Args::TemporalSupersampling_ClampCachedValues_StdDevGamma,
        Denoiser_Args::TemporalSupersampling_ClampCachedValues_MinStdDevTolerance,
        Denoiser_Args::MinTsppToUseTemporalVariance,
        Denoiser_Args::TemporalSupersampling_ClampDifferenceToTsppScale,
        Sample::g_debugOutput,
        Denoiser_Args::LowTsppMaxTspp,
        Denoiser_Args::LowTsppDecayConstant,
        isCheckerboardSamplingEnabled,
        checkerboardLoadEvenPixels);

    // Transition output resource to SRV state.        
    {
        resourceStateTracker->TransitionResource(&m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::Tspp], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resourceStateTracker->TransitionResource(&m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::CoefficientSquaredMean], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resourceStateTracker->TransitionResource(&m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::RayHitDistance], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resourceStateTracker->TransitionResource(&m_varianceResources[AOVarianceResource::Raw], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resourceStateTracker->TransitionResource(&m_multiPassDenoisingBlurStrength, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // ToDo only run when smoothing is enabled
    // Smoothen the variance.
    {
        {
            resourceStateTracker->TransitionResource(&m_varianceResources[AOVarianceResource::Smoothed], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            resourceStateTracker->InsertUAVBarrier(&m_varianceResources[AOVarianceResource::Raw]);
        }

        // ToDo should we be smoothing before temporal?
        // Smoothen the local variance which is prone to error due to undersampled input.
        {
            {
                ScopedTimer _prof(L"Mean Variance Smoothing", commandList);
                resourceStateTracker->FlushResourceBarriers();
                m_gaussianSmoothingKernel.Run(
                    commandList,
                    m_denoisingWidth,
                    m_denoisingHeight,
                    RTAOGpuKernels::GaussianFilter::Filter3x3,
                    m_cbvSrvUavHeap->GetHeap(),
                    m_varianceResources[AOVarianceResource::Raw].gpuDescriptorReadAccess,
                    m_varianceResources[AOVarianceResource::Smoothed].gpuDescriptorWriteAccess);
            }
        }
        resourceStateTracker->TransitionResource(&m_varianceResources[AOVarianceResource::Smoothed], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // ToDo?
    if (fillInMissingValues)
    {
        // Fill in missing/disoccluded values.
        {
            if (isCheckerboardSamplingEnabled)
            {
                bool fillEvenPixels = !checkerboardLoadEvenPixels;
                resourceStateTracker->FlushResourceBarriers();
                m_fillInCheckerboardKernel.Run(
                    commandList,
                    m_cbvSrvUavHeap->GetHeap(),
                    m_denoisingWidth,
                    m_denoisingHeight,
                    TemporalOutCoefficient->gpuDescriptorWriteAccess,
                    fillEvenPixels);
            }
        }
    }
    resourceStateTracker->TransitionResource(TemporalOutCoefficient, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Denoiser::BlurDisocclusions(Pathtracer& pathtracer)
{
    auto commandList = m_deviceResources->GetCommandList();
    auto resourceStateTracker = m_deviceResources->GetGpuResourceStateTracker();

    ScopedTimer _prof(L"Low-Tspp Multi-pass blur", commandList);

    UINT numPasses = static_cast<UINT>(Denoiser_Args::LowTspBlurPasses);
    
    GpuResource* resources[2] = {
        &m_temporalSupersampling_blendedAOCoefficient[0],
        &m_temporalSupersampling_blendedAOCoefficient[1],
    };

    GpuResource* OutResource = &m_temporalAOCoefficient[m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex];

    bool readWriteUAV_and_skipPassthrough = false;// (numPasses % 2) == 1;

    // ToDo remove the flush. It's done to avoid two same resource transitions since prev atrous pass sets the resource to SRV.
    resourceStateTracker->FlushResourceBarriers();
    if (Denoiser_Args::LowTsppUseUAVReadWrite)
    {
        readWriteUAV_and_skipPassthrough = true;
        resourceStateTracker->TransitionResource(OutResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // ToDo test perf win by using 16b Depth over 32b encoded normal depth.
    GpuResource(&GBufferResources)[GBufferResource::Count] = pathtracer.GBufferResources(RTAO_Args::QuarterResAO);

    UINT FilterSteps[4] = {
        1, 4, 8, 16
    };

    UINT filterStep = 1;

    for (UINT i = 0; i < numPasses; i++)
    {
        // ToDo
        //filterStep = FilterSteps[i];
        wstring passName = L"Depth Aware Gaussian Blur with a pixel step " + to_wstring(filterStep);
        ScopedTimer _prof(passName.c_str(), commandList);

        // TODO remove one path
        if (Denoiser_Args::LowTsppUseUAVReadWrite)
        {
            resourceStateTracker->InsertUAVBarrier(OutResource);

            resourceStateTracker->FlushResourceBarriers();
            m_bilateralFilterKernel.Run(
                commandList,
                filterStep,
                Denoiser_Args::LowTsppNormalExponent,
                Denoiser_Args::LowTsppMinNormalWeight,
                m_cbvSrvUavHeap->GetHeap(),
                m_temporalSupersampling_blendedAOCoefficient[0].gpuDescriptorReadAccess,
                GBufferResources[GBufferResource::Depth].gpuDescriptorReadAccess,
                m_multiPassDenoisingBlurStrength.gpuDescriptorReadAccess,
                OutResource,
                readWriteUAV_and_skipPassthrough);
        }
        else
        {
            GpuResource* inResource = i > 0 ? resources[i % 2] : OutResource;
            GpuResource* outResource = i < numPasses - 1 ? resources[(i + 1) % 2] : OutResource;

            {
                resourceStateTracker->TransitionResource(outResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                resourceStateTracker->InsertUAVBarrier(inResource);
            }

            resourceStateTracker->FlushResourceBarriers();
            m_bilateralFilterKernel.Run(
                commandList,
                filterStep,
                Denoiser_Args::LowTsppNormalExponent,
                Denoiser_Args::LowTsppMinNormalWeight,
                m_cbvSrvUavHeap->GetHeap(),
                inResource->gpuDescriptorReadAccess,
                GBufferResources[GBufferResource::Depth].gpuDescriptorReadAccess,
                m_multiPassDenoisingBlurStrength.gpuDescriptorReadAccess,
                outResource,
                readWriteUAV_and_skipPassthrough);

            {
                resourceStateTracker->TransitionResource(outResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                resourceStateTracker->InsertUAVBarrier(outResource);
            }
        }

        filterStep *= 2;
    }


    if (Denoiser_Args::LowTsppUseUAVReadWrite)
    {
        resourceStateTracker->TransitionResource(OutResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resourceStateTracker->InsertUAVBarrier(OutResource);
    }
}


void Denoiser::ApplyAtrousWaveletTransformFilter(Pathtracer& pathtracer, RTAO& rtao)
{
    auto commandList = m_deviceResources->GetCommandList();
    auto resourceStateTracker = m_deviceResources->GetGpuResourceStateTracker();

    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

    GpuResource* AOResources = rtao.AOResources();
    GpuResource* VarianceResource = Denoiser_Args::UseSmoothedVariance ? &m_varianceResources[AOVarianceResource::Smoothed] : &m_varianceResources[AOVarianceResource::Raw];

    // Transition Resources.
    resourceStateTracker->TransitionResource(&AOResources[AOResource::Smoothed], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Adaptive kernel radius rotation.
    float kernelRadiusLerfCoef = 0;
    if (Denoiser_Args::KernelRadius_RotateKernel_Enabled)
    {
        static UINT frameID = 0;
        UINT i = frameID++ % Denoiser_Args::KernelRadius_RotateKernel_NumCycles;
        kernelRadiusLerfCoef = i / static_cast<float>(Denoiser_Args::KernelRadius_RotateKernel_NumCycles);
    }

    float ValueSigma;
    float NormalSigma;
    float DepthSigma;
        ValueSigma = Denoiser_Args::AODenoiseValueSigma;
        NormalSigma = Denoiser_Args::AODenoiseNormalSigma;
        DepthSigma = Denoiser_Args::AODenoiseDepthSigma;


    UINT numFilterPasses = Denoiser_Args::AtrousFilterPasses;

    bool cacheIntermediateDenoiseOutput =
        Denoiser_Args::TemporalSupersampling_CacheDenoisedOutput &&
        static_cast<UINT>(Denoiser_Args::TemporalSupersampling_CacheDenoisedOutputPassNumber) < numFilterPasses;

    GpuResource(&GBufferResources)[GBufferResource::Count] = pathtracer.GBufferResources(RTAO_Args::QuarterResAO);
    GpuResource* InputAOCoefficientResource = &m_temporalAOCoefficient[m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex];
    GpuResource* OutputIntermediateResource = nullptr;
    if (cacheIntermediateDenoiseOutput)
    {
        // ToDo clean this up so that its clear.
        m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex = (m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex + 1) % 2;
        OutputIntermediateResource = &m_temporalAOCoefficient[m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex];
    }

    if (OutputIntermediateResource)
    {
        resourceStateTracker->TransitionResource(OutputIntermediateResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }


    float staleNeighborWeightScale = Denoiser_Args::LowerWeightForStaleSamples ? RTAO_Args::Spp : 1.f;
    bool forceDenoisePass = Denoiser_Args::ForceDenoisePass;

    if (forceDenoisePass)
    {
        Denoiser_Args::ForceDenoisePass.Bang();
    }
    // A-trous edge-preserving wavelet tranform filter
    if (numFilterPasses > 0) // ToDo remove
    {
        // Adjust factors that change based on max ray hit distance.
        // Values were empirically found.
        float RayHitDistanceScaleFactor = 22 / RTAO_Args::MaxRayHitTime * Denoiser_Args::AdaptiveKernelSize_RayHitDistanceScaleFactor;
        float RayHitDistanceScaleExponent = lerp(1, Denoiser_Args::AdaptiveKernelSize_RayHitDistanceScaleExponent, relativeCoef(RTAO_Args::MaxRayHitTime, 4, 22));

        ScopedTimer _prof(L"AtrousWaveletTransformFilter", commandList);
        resourceStateTracker->FlushResourceBarriers();
        // ToDo trim obsolete
        m_atrousWaveletTransformFilter.Run(
            commandList,
            m_cbvSrvUavHeap->GetHeap(),
            static_cast<RTAOGpuKernels::AtrousWaveletTransformCrossBilateralFilter::FilterType>(static_cast<UINT>(Denoiser_Args::Mode)),
            InputAOCoefficientResource->gpuDescriptorReadAccess,
            GBufferResources[GBufferResource::SurfaceNormalDepth].gpuDescriptorReadAccess,
            VarianceResource->gpuDescriptorReadAccess,
            m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::RayHitDistance].gpuDescriptorReadAccess,
            GBufferResources[GBufferResource::PartialDepthDerivatives].gpuDescriptorReadAccess,
            m_temporalCache[m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::Tspp].gpuDescriptorReadAccess,
            &AOResources[AOResource::Smoothed],
            OutputIntermediateResource,
            &Sample::g_debugOutput[0],
            &Sample::g_debugOutput[1],
            ValueSigma,
            DepthSigma,
            NormalSigma,
            Denoiser_Args::WeightScale,
            static_cast<UINT>(Denoiser_Args::TemporalSupersampling_CacheDenoisedOutputPassNumber),
            numFilterPasses,
            RTAOGpuKernels::AtrousWaveletTransformCrossBilateralFilter::Mode::OutputFilteredValue,
            Denoiser_Args::PerspectiveCorrectDepthInterpolation,
            Denoiser_Args::UseAdaptiveKernelSize,
            kernelRadiusLerfCoef,
            RayHitDistanceScaleFactor,
            RayHitDistanceScaleExponent,
            Denoiser_Args::FilterMinKernelWidth,
            static_cast<UINT>((Denoiser_Args::FilterMaxKernelWidthPercentage / 100) * m_denoisingWidth),
            Denoiser_Args::FilterVarianceSigmaScaleOnSmallKernels,
            RTAO_Args::QuarterResAO,
            Denoiser_Args::MinVarianceToDenoise,
            staleNeighborWeightScale,
            Denoiser_Args::AODenoiseDepthWeightCutoff,
            forceDenoisePass,
            Denoiser_Args::FilterWeightByTspp);
    }

    resourceStateTracker->TransitionResource(&AOResources[AOResource::Smoothed], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    resourceStateTracker->TransitionResource(OutputIntermediateResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
