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
#include "EngineProfiling.h"
#include "RTAOGpuKernels.h"
#include "DirectXRaytracingHelper.h"
#include "D3D12RaytracingAmbientOcclusion.h"
#include "CompiledShaders\GaussianFilter3x3CS.hlsl.h"
#include "CompiledShaders\GaussianFilterRG3x3CS.hlsl.h"
#include "CompiledShaders\EdgeStoppingAtrousWaveletTransfromCrossBilateralFilter_Gaussian3x3CS.hlsl.h"
#include "CompiledShaders\EdgeStoppingAtrousWaveletTransfromCrossBilateralFilter_Gaussian5x5CS.hlsl.h"
#include "CompiledShaders\CalculateMeanVariance_SeparableFilterCS.hlsl.h"
#include "CompiledShaders\CalculateMeanVariance_SeparableFilterCS_AnyToAnyWaveReadLaneAt.hlsl.h"
#include "CompiledShaders\CalculateMeanVariance_SeparableFilterCS_CheckerboardSampling_AnyToAnyWaveReadLaneAt.hlsl.h"
#include "CompiledShaders\TemporalSupersampling_BlendWithCurrentFrameCS.hlsl.h"
#include "CompiledShaders\TemporalSupersampling_ReverseReprojectCS.hlsl.h"
#include "CompiledShaders\CountingSort_SortRays_64x128rayGroupCS.hlsl.h"
#include "CompiledShaders\AORayGenCS.hlsl.h"
#include "CompiledShaders\DepthAwareSeparableGaussianFilterCS_AnyToAnyWaveReadLaneAt.hlsl.h"
#include "CompiledShaders\NormalDepthAwareSeparableGaussianFilterCS_AnyToAnyWaveReadLaneAt.hlsl.h"
#include "CompiledShaders\FillInCheckerboard_CrossBox4TapFilterCS.hlsl.h"

using namespace std;

namespace RTAOGpuKernels
{
    namespace RootSignature {
        namespace GaussianFilter {
            namespace Slot {
                enum Enum {
                    Output = 0,
                    Input,
                    ConstantBuffer,
                    Count
                };
            }
        }
    }

    void GaussianFilter::Initialize(ID3D12Device5* device, UINT frameCount, UINT numCallsPerFrame)
    {
        // Create root signature.
        {
            using namespace RootSignature::GaussianFilter;

            CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count];
            ranges[Slot::Input].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);  // 1 input texture
            ranges[Slot::Output].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);  // 1 output texture

            CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
            rootParameters[Slot::Input].InitAsDescriptorTable(1, &ranges[Slot::Input]);
            rootParameters[Slot::Output].InitAsDescriptorTable(1, &ranges[Slot::Output]);
            rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);

            CD3DX12_STATIC_SAMPLER_DESC staticSampler(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_MIRROR);

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters, 1, &staticSampler);
            SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_rootSignature, L"Compute root signature: GaussianFilter");
        }

        // Create compute pipeline state.
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
            descComputePSO.pRootSignature = m_rootSignature.Get();

            for (UINT i = 0; i < FilterType::Count; i++)
            {
                switch (i)
                {
                case Filter3x3:
                    descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pGaussianFilter3x3CS), ARRAYSIZE(g_pGaussianFilter3x3CS));
                    break;
                case Filter3x3RG:
                    descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pGaussianFilterRG3x3CS), ARRAYSIZE(g_pGaussianFilterRG3x3CS));
                    break;
                }

                ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_pipelineStateObjects[i])));
                m_pipelineStateObjects[i]->SetName(L"Pipeline state object: GaussianFilter");
            }
        }

        // Create shader resources
        {
            m_CB.Create(device, frameCount * numCallsPerFrame, L"Constant Buffer: GaussianFilter");
        }
    }


    // Blurs input resource with a Gaussian filter.
    // width, height - dimensions of the input resource.
    void GaussianFilter::Run(
        ID3D12GraphicsCommandList4* commandList,
        UINT width,
        UINT height,
        FilterType type,
        ID3D12DescriptorHeap* descriptorHeap,
        D3D12_GPU_DESCRIPTOR_HANDLE inputResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE outputResourceHandle)
    {
        using namespace RootSignature::GaussianFilter;
        using namespace DefaultComputeShaderParams;

        ScopedTimer _prof(L"GaussianFilter", commandList);

        m_CB->textureDim = XMUINT2(width, height);
        m_CB->invTextureDim = XMFLOAT2(1.f / width, 1.f / height);
        m_CBinstanceID = (m_CBinstanceID + 1) % m_CB.NumInstances();
        m_CB.CopyStagingToGpu(m_CBinstanceID);

        // Set pipeline state.
        {
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetComputeRootDescriptorTable(Slot::Input, inputResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::Output, outputResourceHandle);
            commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_CB.GpuVirtualAddress(m_CBinstanceID));
            commandList->SetPipelineState(m_pipelineStateObjects[type].Get());
        }

        // Dispatch.
        XMUINT2 groupSize(CeilDivide(width, ThreadGroup::Width), CeilDivide(height, ThreadGroup::Height));
        commandList->Dispatch(groupSize.x, groupSize.y, 1);
    }

    namespace RootSignature {
        namespace BilateralFilter {
            namespace Slot {
                enum Enum {
                    Output = 0,
                    Input,
                    Depth,
                    BlurStrength,
                    Debug1,
                    Debug2,
                    ConstantBuffer,
                    Count
                };
            }
        }
    }

    void BilateralFilter::Initialize(ID3D12Device5* device, UINT frameCount, UINT numCallsPerFrame)
    {
        // Create root signature.
        {
            using namespace RootSignature::BilateralFilter;

            CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count];
            ranges[Slot::Input].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            ranges[Slot::Depth].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
            ranges[Slot::BlurStrength].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
            ranges[Slot::Output].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            ranges[Slot::Debug1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3);
            ranges[Slot::Debug2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4);

            CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
            rootParameters[Slot::Input].InitAsDescriptorTable(1, &ranges[Slot::Input]);
            rootParameters[Slot::Depth].InitAsDescriptorTable(1, &ranges[Slot::Depth]);
            rootParameters[Slot::BlurStrength].InitAsDescriptorTable(1, &ranges[Slot::BlurStrength]);
            rootParameters[Slot::Output].InitAsDescriptorTable(1, &ranges[Slot::Output]);
            rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);
            rootParameters[Slot::Debug1].InitAsDescriptorTable(1, &ranges[Slot::Debug1]);
            rootParameters[Slot::Debug2].InitAsDescriptorTable(1, &ranges[Slot::Debug2]);

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_rootSignature, L"Compute root signature: BilateralFilter");
        }

        // Create compute pipeline state.
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
            descComputePSO.pRootSignature = m_rootSignature.Get();

            for (UINT i = 0; i < FilterType::Count; i++)
            {
                switch (i)
                {
                case DepthAware_GaussianFilter5x5:
                    descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pDepthAwareSeparableGaussianFilterCS_AnyToAnyWaveReadLaneAt), ARRAYSIZE(g_pDepthAwareSeparableGaussianFilterCS_AnyToAnyWaveReadLaneAt));
                    break;
                case NormalDepthAware_GaussianFilter5x5:
                    descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pNormalDepthAwareSeparableGaussianFilterCS_AnyToAnyWaveReadLaneAt), ARRAYSIZE(g_pNormalDepthAwareSeparableGaussianFilterCS_AnyToAnyWaveReadLaneAt));
                    break;
                }
                ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_pipelineStateObjects[i])));
                m_pipelineStateObjects[i]->SetName(L"Pipeline state object: BilateralFilter");
            }
        }

        // Create shader resources
        {
            m_CB.Create(device, frameCount * numCallsPerFrame, L"Constant Buffer: BilateralFilter");
        }
    }


    // Blurs input resource with a Gaussian filter.
    // width, height - dimensions of the input resource.
    void BilateralFilter::Run(
        ID3D12GraphicsCommandList4* commandList,
        FilterType type,
        UINT filterStep,
        float normalWeightExponent,
        float minNormalWeightStrength,
        ID3D12DescriptorHeap* descriptorHeap,
        D3D12_GPU_DESCRIPTOR_HANDLE inputResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputDepthResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputBlurStrengthResourceHandle,
        GpuResource* outputResource,
        bool readWriteUAV_and_skipPassthrough)
    {
        using namespace RootSignature::BilateralFilter;
        using namespace DefaultComputeShaderParams;

        ScopedTimer _prof(L"BilateralFilter", commandList);

        auto resourceDesc = outputResource->resource.Get()->GetDesc();
        XMUINT2 resourceDim(static_cast<UINT>(resourceDesc.Width), static_cast<UINT>(resourceDesc.Height));

        m_CB->textureDim = resourceDim;
        m_CB->step = filterStep;
        m_CB->readWriteUAV_and_skipPassthrough = readWriteUAV_and_skipPassthrough;
        m_CB->normalWeightExponent = normalWeightExponent;
        m_CB->minNormalWeightStrength = minNormalWeightStrength;
        m_CBinstanceID = (m_CBinstanceID + 1) % m_CB.NumInstances();
        m_CB.CopyStagingToGpu(m_CBinstanceID);

        // Set pipeline state.
        {
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetComputeRootDescriptorTable(Slot::Input, inputResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::Depth, inputDepthResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::BlurStrength, inputBlurStrengthResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::Output, outputResource->gpuDescriptorWriteAccess);
            commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_CB.GpuVirtualAddress(m_CBinstanceID));

            GpuResource* debugResources = Sample::g_debugOutput;
            commandList->SetComputeRootDescriptorTable(Slot::Debug1, debugResources[0].gpuDescriptorWriteAccess);
            commandList->SetComputeRootDescriptorTable(Slot::Debug2, debugResources[1].gpuDescriptorWriteAccess);

            commandList->SetPipelineState(m_pipelineStateObjects[type].Get());
        }

        // Account for interleaved Group execution
        UINT widthCS = filterStep * ThreadGroup::Width * CeilDivide(resourceDim.x, filterStep * ThreadGroup::Width);
        UINT heightCS = filterStep * ThreadGroup::Height * CeilDivide(resourceDim.y, filterStep * ThreadGroup::Height);

        // Dispatch.
        XMUINT2 groupSize(CeilDivide(widthCS, ThreadGroup::Width), CeilDivide(heightCS, ThreadGroup::Height));
        commandList->Dispatch(groupSize.x, groupSize.y, 1);
    }


    namespace RootSignature {
        namespace AtrousWaveletTransformCrossBilateralFilter {
            namespace Slot {
                enum Enum {
                    Output = 0,
                    VarianceOutput,
                    Input,
                    Normals,
                    Variance,
                    SmoothedVariance,
                    RayHitDistance,
                    PartialDistanceDerivatives,
                    Trpp,
                    ConstantBuffer,
                    Debug1,
                    Debug2,
                    Count
                };
            }
        }
    }

    void AtrousWaveletTransformCrossBilateralFilter::Initialize(ID3D12Device5* device, UINT frameCount, UINT maxFilterPasses, UINT numCallsPerFrame)
    {
        // Create root signature.
        {
            using namespace RootSignature::AtrousWaveletTransformCrossBilateralFilter;

            CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count];
            ranges[Slot::Input].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            ranges[Slot::Normals].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
            ranges[Slot::Variance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);
            ranges[Slot::SmoothedVariance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);
            ranges[Slot::RayHitDistance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);
            ranges[Slot::PartialDistanceDerivatives].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7);
            ranges[Slot::Trpp].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8);
            ranges[Slot::Output].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            ranges[Slot::VarianceOutput].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
            ranges[Slot::Debug1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3);
            ranges[Slot::Debug2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4);


            CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
            rootParameters[Slot::Input].InitAsDescriptorTable(1, &ranges[Slot::Input]);
            rootParameters[Slot::Normals].InitAsDescriptorTable(1, &ranges[Slot::Normals]);
            rootParameters[Slot::Variance].InitAsDescriptorTable(1, &ranges[Slot::Variance]);
            rootParameters[Slot::SmoothedVariance].InitAsDescriptorTable(1, &ranges[Slot::SmoothedVariance]);
            rootParameters[Slot::Output].InitAsDescriptorTable(1, &ranges[Slot::Output]);
            rootParameters[Slot::VarianceOutput].InitAsDescriptorTable(1, &ranges[Slot::VarianceOutput]);
            rootParameters[Slot::RayHitDistance].InitAsDescriptorTable(1, &ranges[Slot::RayHitDistance]);
            rootParameters[Slot::PartialDistanceDerivatives].InitAsDescriptorTable(1, &ranges[Slot::PartialDistanceDerivatives]);
            rootParameters[Slot::Trpp].InitAsDescriptorTable(1, &ranges[Slot::Trpp]);
            rootParameters[Slot::Debug1].InitAsDescriptorTable(1, &ranges[Slot::Debug1]);
            rootParameters[Slot::Debug2].InitAsDescriptorTable(1, &ranges[Slot::Debug2]);
            rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_rootSignature, L"Compute root signature: AtrousWaveletTransformCrossBilateralFilter");
        }

        // Create compute pipeline state.
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
            descComputePSO.pRootSignature = m_rootSignature.Get();

            for (UINT i = 0; i < FilterType::Count; i++)
            {
                switch (i)
                {
                case EdgeStoppingGaussian5x5:
                    descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pEdgeStoppingAtrousWaveletTransfromCrossBilateralFilter_Gaussian5x5CS), ARRAYSIZE(g_pEdgeStoppingAtrousWaveletTransfromCrossBilateralFilter_Gaussian5x5CS));
                    break;
                case EdgeStoppingGaussian3x3:
                    descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pEdgeStoppingAtrousWaveletTransfromCrossBilateralFilter_Gaussian3x3CS), ARRAYSIZE(g_pEdgeStoppingAtrousWaveletTransfromCrossBilateralFilter_Gaussian3x3CS));
                    break;
                }

                ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_pipelineStateObjects[i])));
                m_pipelineStateObjects[i]->SetName(L"Pipeline state object: AtrousWaveletTransformCrossBilateralFilter");
            }
        }

        // Create shader resources.
        {
            m_maxFilterPasses = maxFilterPasses;
            UINT numInstancesPerFrame = maxFilterPasses * numCallsPerFrame;
            m_CB.Create(device, frameCount * numInstancesPerFrame, L"Constant Buffer: AtrousWaveletTransformCrossBilateralFilter");
        }
    }

    void AtrousWaveletTransformCrossBilateralFilter::CreateInputResourceSizeDependentResources(
        ID3D12Device5* device,
        DX::DescriptorHeap* descriptorHeap,
        UINT width,
        UINT height,
        DXGI_FORMAT format)
    {
        // Create shader resources
        {
            CreateRenderTargetResource(device, format, width, height, descriptorHeap,
                &m_intermediateValueOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"UAV texture: AtrousWaveletTransformCrossBilateralFilter intermediate value output");

            CreateRenderTargetResource(device, format, width, height, descriptorHeap,
                &m_intermediateVarianceOutputs[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"UAV texture: AtrousWaveletTransformCrossBilateralFilter intermediate variance output 0");

            CreateRenderTargetResource(device, format, width, height, descriptorHeap,
                &m_intermediateVarianceOutputs[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"UAV texture: AtrousWaveletTransformCrossBilateralFilter intermediate variance output 1");
        }
    }

    // Expects, and returns outputResource in D3D12_RESOURCE_STATE_UNORDERED_ACCESS state.
    // Expects, and returns outputIntermediateResource in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE state.
    void AtrousWaveletTransformCrossBilateralFilter::Run(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12DescriptorHeap* descriptorHeap,
        FilterType filterType,
        D3D12_GPU_DESCRIPTOR_HANDLE inputValuesResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputNormalsResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputVarianceResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputHitDistanceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputPartialDistanceDerivativesResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputTrppResourceHandle,
        GpuResource* outputResource,
        GpuResource* outputIntermediateResource,
        GpuResource* outputDebug1Resource,
        GpuResource* outputDebug2Resource,
        float valueSigma,
        float depthSigma,
        float normalSigma,
        float weightScale,
        UINT kernelStepShifts[5],
        UINT passNumberToOutputToIntermediateResource,
        UINT numFilterPasses,
        Mode filterMode,
        bool useCalculatedVariance,
        bool perspectiveCorrectDepthInterpolation,
        bool useAdaptiveKernelSize,
        float minHitDistanceToKernelWidthScale,
        UINT minKernelWidth,
        UINT maxKernelWidth,
        float varianceSigmaScaleOnSmallKernels,
        bool usingBilateralDownsampledBuffers,
        float minVarianceToDenoise,
        float staleNeighborWeightScale,
        float depthWeightCutoff,
        bool useProjectedDepthTest,
        bool forceDenoisePass,
        bool weightByTrpp)
    {

        // ToDo: cleanup use of variance
        // SmoothedVariance is used for edge stopping
        // Variance is used for intermediate variance calculations for the 

        using namespace RootSignature::AtrousWaveletTransformCrossBilateralFilter;
        using namespace AtrousWaveletTransformFilterCS;

        ScopedTimer _prof(L"AtrousWaveletTransformCrossBilateralFilter", commandList);

        m_CBinstanceID = ((m_CBinstanceID + 1) * m_maxFilterPasses) % m_CB.NumInstances();

        // Set pipeline state.
        {
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(m_pipelineStateObjects[filterType].Get());
            commandList->SetComputeRootDescriptorTable(Slot::Normals, inputNormalsResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::Variance, inputVarianceResourceHandle);
            // ToDo Smoothen input variance or remove the dupe
            commandList->SetComputeRootDescriptorTable(Slot::SmoothedVariance, inputVarianceResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::RayHitDistance, inputHitDistanceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::PartialDistanceDerivatives, inputPartialDistanceDerivativesResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::Trpp, inputTrppResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::Debug1, outputDebug1Resource->gpuDescriptorWriteAccess);
            commandList->SetComputeRootDescriptorTable(Slot::Debug2, outputDebug2Resource->gpuDescriptorWriteAccess);
        }

        auto resourceDesc = outputResource->resource.Get()->GetDesc();
        XMUINT2 resourceDim(static_cast<UINT>(resourceDesc.Width), static_cast<UINT>(resourceDesc.Height));

        // Update the Constant Buffers.
        for (UINT i = 0; i < numFilterPasses; i++)
        {
            // Ref: Dammertz2010
            // Tighten value range smoothing for higher passes.

            if (useCalculatedVariance)
            {
                m_CB->valueSigma = valueSigma;
            }
            else
            {
                m_CB->valueSigma = i > 0 ? valueSigma * powf(2.f, -float(i)) : 1;
            }
            m_CB->depthSigma = depthSigma;
            m_CB->normalSigma = normalSigma;
            m_CB->weightScale = weightScale;
#if RAYTRACING_MANUAL_KERNEL_STEP_SHIFTS
            m_CB->kernelStepShift = kernelStepShifts[i];
#else
            m_CB->kernelStepShift = i;
#endif
            // Move vars not changing inside loop outside of it.
            m_CB->useCalculatedVariance = filterMode == OutputFilteredValue && useCalculatedVariance;
            m_CB->outputFilteredVariance = numFilterPasses > 1 && filterMode == OutputFilteredValue && useCalculatedVariance;
            m_CB->outputFilteredValue = filterMode == OutputFilteredValue;
            m_CB->outputFilterWeightSum = filterMode == OutputPerPixelFilterWeightSum;
            m_CB->perspectiveCorrectDepthInterpolation = perspectiveCorrectDepthInterpolation;
            m_CB->useAdaptiveKernelSize = useAdaptiveKernelSize;
            m_CB->minHitDistanceToKernelWidthScale = minHitDistanceToKernelWidthScale;
            m_CB->minKernelWidth = minKernelWidth;
            m_CB->maxKernelWidth = maxKernelWidth;
            m_CB->varianceSigmaScaleOnSmallKernels = varianceSigmaScaleOnSmallKernels;
            m_CB->usingBilateralDownsampledBuffers = usingBilateralDownsampledBuffers;
            m_CB->textureDim = resourceDim;
            m_CB->minVarianceToDenoise = minVarianceToDenoise;
            m_CB->staleNeighborWeightScale = i == 0 ? staleNeighborWeightScale : 1;  // ToDo revise
            m_CB->depthWeightCutoff = depthWeightCutoff;
            m_CB->useProjectedDepthTest = useProjectedDepthTest;
            m_CB->forceDenoisePass = forceDenoisePass;
            m_CB->DepthNumMantissaBits = NumMantissaBitsInFloatFormat(16);
            m_CB.CopyStagingToGpu(m_CBinstanceID + i);
        }

        //
        // Iterative filter
        //
        {
            XMUINT2 groupSize(CeilDivide(resourceDim.x, ThreadGroup::Width), CeilDivide(resourceDim.y, ThreadGroup::Height));

            // Order the resources such that the final pass writes to outputResource.
            GpuResource* outValueResources[2] =
            {
                numFilterPasses % 2 == 1 ? outputResource : &m_intermediateValueOutput,
                numFilterPasses % 2 == 1 ? &m_intermediateValueOutput : outputResource,
            };


            if (numFilterPasses == 1 && outputIntermediateResource && 0 == passNumberToOutputToIntermediateResource)
            {
                outValueResources[0] = outputIntermediateResource;
            }

            // First iteration reads from input resource.		
            commandList->SetComputeRootDescriptorTable(Slot::Input, inputValuesResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::Output, outValueResources[0]->gpuDescriptorWriteAccess);
            commandList->SetComputeRootDescriptorTable(Slot::VarianceOutput, m_intermediateVarianceOutputs[0].gpuDescriptorWriteAccess);

            for (UINT i = 0; i < numFilterPasses; i++)
            {
                commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_CB.GpuVirtualAddress(m_CBinstanceID + i));

                // Dispatch.
                commandList->Dispatch(groupSize.x, groupSize.y, 1);

                // ToDo remove the copy, write directly to the resource instead.
                if (i > 0 && outputIntermediateResource && i == passNumberToOutputToIntermediateResource)
                {
                    commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(outValueResources[i % 2]->resource.Get()));

                    CopyTextureRegion(
                        commandList,
                        outValueResources[i % 2]->resource.Get(),
                        outputIntermediateResource->resource.Get(),
                        &CD3DX12_BOX(0, 0, resourceDim.x, resourceDim.y),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }

                // Transition and bind resources for the next pass.
                // Flip input/output. 
                if (i < numFilterPasses - 1)
                {
                    // Ping-pong input/output resources across passes.
                    UINT inputID = i % 2;
                    UINT outputID = (i + 1) % 2;

                    D3D12_RESOURCE_BARRIER barriers[4] = {
                        CD3DX12_RESOURCE_BARRIER::Transition(m_intermediateVarianceOutputs[inputID].resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                        CD3DX12_RESOURCE_BARRIER::Transition(outValueResources[inputID]->resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),

                        CD3DX12_RESOURCE_BARRIER::Transition(m_intermediateVarianceOutputs[outputID].resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                        CD3DX12_RESOURCE_BARRIER::Transition(outValueResources[outputID]->resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    };
                    // Only flip outputs from pass 0 as the caller passed in resources were used as input.
                    commandList->ResourceBarrier(i == 0 ? 2 : ARRAYSIZE(barriers), barriers);

                    // Flip input, output resources.
                    commandList->SetComputeRootDescriptorTable(Slot::Input, outValueResources[inputID]->gpuDescriptorReadAccess);
                    commandList->SetComputeRootDescriptorTable(Slot::Output, outValueResources[outputID]->gpuDescriptorWriteAccess);
                    commandList->SetComputeRootDescriptorTable(Slot::Variance, m_intermediateVarianceOutputs[inputID].gpuDescriptorReadAccess);
                    commandList->SetComputeRootDescriptorTable(Slot::SmoothedVariance, m_intermediateVarianceOutputs[inputID].gpuDescriptorReadAccess);
                    commandList->SetComputeRootDescriptorTable(Slot::VarianceOutput, m_intermediateVarianceOutputs[outputID].gpuDescriptorWriteAccess);
                }
            }

            // Transition the intermediate output resource back.
            if (numFilterPasses > 1)
            {
                bool isVar0ResourceInUAVState = ((numFilterPasses - 1) % 2) == 0;
                D3D12_RESOURCE_BARRIER barriers[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(m_intermediateValueOutput.resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    CD3DX12_RESOURCE_BARRIER::Transition(m_intermediateVarianceOutputs[isVar0ResourceInUAVState ? 1 : 0].resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                };
                // Transition variance resources back only if they're not in their default state.
                commandList->ResourceBarrier(ARRAYSIZE(barriers), barriers);
            }
        }
    }


    namespace RootSignature {
        namespace CalculateMeanVariance {
            namespace Slot {
                enum Enum {
                    OutputMeanVariance = 0,
                    Input,
                    Debug1,
                    Debug2,
                    ConstantBuffer,
                    Count
                };
            }
        }
    }

    void CalculateMeanVariance::Initialize(ID3D12Device5* device, UINT frameCount, UINT numCallsPerFrame)
    {
        // Create root signature.
        {
            using namespace RootSignature::CalculateMeanVariance;

            CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count];
            ranges[Slot::Input].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            ranges[Slot::OutputMeanVariance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            ranges[Slot::Debug1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3);
            ranges[Slot::Debug2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4);

            CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
            rootParameters[Slot::Input].InitAsDescriptorTable(1, &ranges[Slot::Input]);
            rootParameters[Slot::OutputMeanVariance].InitAsDescriptorTable(1, &ranges[Slot::OutputMeanVariance]);
            rootParameters[Slot::Debug1].InitAsDescriptorTable(1, &ranges[Slot::Debug1]);
            rootParameters[Slot::Debug2].InitAsDescriptorTable(1, &ranges[Slot::Debug2]);
            rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_rootSignature, L"Compute root signature: CalculateMeanVariance");
        }

        // Create compute pipeline state.
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
            descComputePSO.pRootSignature = m_rootSignature.Get();
            descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pCalculateMeanVariance_SeparableFilterCS_CheckerboardSampling_AnyToAnyWaveReadLaneAt), ARRAYSIZE(g_pCalculateMeanVariance_SeparableFilterCS_CheckerboardSampling_AnyToAnyWaveReadLaneAt));


            ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_pipelineStateObject)));
            m_pipelineStateObject->SetName(L"Pipeline state object: CalculateMeanVariance");
        }

        // Create shader resources.
        {
            m_CB.Create(device, frameCount * numCallsPerFrame, L"Constant Buffer: CalculateMeanVariance");
        }
    }

    // Expects, and returns, outputResource in D3D12_RESOURCE_STATE_UNORDERED_ACCESS state.
    void CalculateMeanVariance::Run(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12DescriptorHeap* descriptorHeap,
        UINT width,
        UINT height,
        D3D12_GPU_DESCRIPTOR_HANDLE inputValuesResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE outputMeanVarianceResourceHandle,
        UINT kernelWidth,
        bool doCheckerboardSampling,
        bool checkerboardLoadEvenPixels)
    {
        using namespace RootSignature::CalculateMeanVariance;
        using namespace DefaultComputeShaderParams;

        ThrowIfFalse((kernelWidth & 1) == 1 && kernelWidth <= 9, L"KernelWidth must be an odd number so that width == radius + 1 + radius");

        ScopedTimer _prof(L"CalculateMeanVariance", commandList);

        // Set pipeline state.
        {
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(m_pipelineStateObject.Get());
            commandList->SetComputeRootDescriptorTable(Slot::Input, inputValuesResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputMeanVariance, outputMeanVarianceResourceHandle);

            GpuResource* debugResources = Sample::g_debugOutput;
            commandList->SetComputeRootDescriptorTable(Slot::Debug1, debugResources[0].gpuDescriptorWriteAccess);
            commandList->SetComputeRootDescriptorTable(Slot::Debug2, debugResources[1].gpuDescriptorWriteAccess);
        }

        // Update the Constant Buffer.
        m_CB->textureDim = XMUINT2(width, height);
        m_CB->kernelWidth = kernelWidth;
        m_CB->kernelRadius = kernelWidth >> 1;
        m_CB->doCheckerboardSampling = doCheckerboardSampling;
        m_CB->pixelStepY = doCheckerboardSampling ? 2 : 1;
        m_CB->areEvenPixelsActive = checkerboardLoadEvenPixels;
        m_CBinstanceID = (m_CBinstanceID + 1) % m_CB.NumInstances();
        m_CB.CopyStagingToGpu(m_CBinstanceID);
        commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_CB.GpuVirtualAddress(m_CBinstanceID));

        // Dispatch.
        {
            XMUINT2 groupSize(CeilDivide(width, ThreadGroup::Width), CeilDivide(height, ThreadGroup::Height * m_CB->pixelStepY));
            commandList->Dispatch(groupSize.x, groupSize.y, 1);
        }
    }


    namespace RootSignature {
        namespace FillInCheckerboard {
            namespace Slot {
                enum Enum {
                    InputOutput = 0,
                    Debug1,
                    Debug2,
                    ConstantBuffer,
                    Count
                };
            }
        }
    }

    void FillInCheckerboard::Initialize(ID3D12Device5* device, UINT frameCount, UINT numCallsPerFrame)
    {
        // Create root signature.
        {
            using namespace RootSignature::FillInCheckerboard;

            CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count];
            ranges[Slot::InputOutput].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            ranges[Slot::Debug1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3);
            ranges[Slot::Debug2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4);

            CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
            rootParameters[Slot::InputOutput].InitAsDescriptorTable(1, &ranges[Slot::InputOutput]);
            rootParameters[Slot::Debug1].InitAsDescriptorTable(1, &ranges[Slot::Debug1]);
            rootParameters[Slot::Debug2].InitAsDescriptorTable(1, &ranges[Slot::Debug2]);
            rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_rootSignature, L"Compute root signature: FillInCheckerboard");
        }
        // Create compute pipeline state.
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
            descComputePSO.pRootSignature = m_rootSignature.Get();
            descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pFillInCheckerboard_CrossBox4TapFilterCS), ARRAYSIZE(g_pFillInCheckerboard_CrossBox4TapFilterCS));
            ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_pipelineStateObject)));
            m_pipelineStateObject->SetName(L"Pipeline state object: FillInCheckerboard");
        }

        // Create shader resources.
        {
            m_CB.Create(device, frameCount * numCallsPerFrame, L"Constant Buffer: FillInCheckerboard");
        }
    }

    // Expects, and returns, outputResource in D3D12_RESOURCE_STATE_UNORDERED_ACCESS state.
    void FillInCheckerboard::Run(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12DescriptorHeap* descriptorHeap,
        UINT width,
        UINT height,
        D3D12_GPU_DESCRIPTOR_HANDLE inputOutputResourceHandle,
        bool fillEvenPixels)
    {
        using namespace RootSignature::FillInCheckerboard;
        using namespace DefaultComputeShaderParams;

        ScopedTimer _prof(L"FillInCheckerboard", commandList);

        // Set pipeline state.
        {
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(m_pipelineStateObject.Get());
            commandList->SetComputeRootDescriptorTable(Slot::InputOutput, inputOutputResourceHandle);

            GpuResource* debugResources = Sample::g_debugOutput;
            commandList->SetComputeRootDescriptorTable(Slot::Debug1, debugResources[0].gpuDescriptorWriteAccess);
            commandList->SetComputeRootDescriptorTable(Slot::Debug2, debugResources[1].gpuDescriptorWriteAccess);
        }

        // Update the Constant Buffer.
        m_CB->textureDim = XMUINT2(width, height);
        m_CB->areEvenPixelsActive = !fillEvenPixels;
        m_CBinstanceID = (m_CBinstanceID + 1) % m_CB.NumInstances();
        m_CB.CopyStagingToGpu(m_CBinstanceID);
        commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_CB.GpuVirtualAddress(m_CBinstanceID));

        // Dispatch.
        XMUINT2 groupSize(CeilDivide(width, ThreadGroup::Width), CeilDivide(height, ThreadGroup::Height * 2));
        commandList->Dispatch(groupSize.x, groupSize.y, 1);
    }


    namespace RootSignature {
        namespace TemporalSupersampling_ReverseReproject {
            namespace Slot {
                enum Enum {
                    OutputCacheTrpp = 0,
                    OutputReprojectedCacheValues,
                    InputCurrentFrameNormalDepth,
                    InputCurrentFrameLinearDepthDerivative,
                    InputReprojectedNormalDepth,
                    InputTextureSpaceMotionVector,      // Texture space motion vector from the previous to the current frame.
                    InputCachedValue,
                    InputCachedNormalDepth,
                    InputCachedTrpp,
                    InputCachedSquaredMeanValue,
                    InputCachedRayHitDistance,
                    OutputDebug1,
                    OutputDebug2,
                    ConstantBuffer,
                    Count
                };
            }
        }
    }

    void TemporalSupersampling_ReverseReproject::Initialize(ID3D12Device5* device, UINT frameCount, UINT numCallsPerFrame)
    {
        // Create root signature.
        {
            using namespace RootSignature::TemporalSupersampling_ReverseReproject;

            CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count];
            ranges[Slot::InputCurrentFrameNormalDepth].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            ranges[Slot::InputCurrentFrameLinearDepthDerivative].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
            ranges[Slot::InputReprojectedNormalDepth].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
            ranges[Slot::InputTextureSpaceMotionVector].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
            ranges[Slot::InputCachedNormalDepth].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);
            ranges[Slot::InputCachedValue].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);
            ranges[Slot::InputCachedTrpp].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);
            ranges[Slot::InputCachedSquaredMeanValue].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7);
            ranges[Slot::InputCachedRayHitDistance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8);

            ranges[Slot::OutputCacheTrpp].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            ranges[Slot::OutputReprojectedCacheValues].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);

            ranges[Slot::OutputDebug1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 10);
            ranges[Slot::OutputDebug2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 11);

            CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
            rootParameters[Slot::InputCurrentFrameNormalDepth].InitAsDescriptorTable(1, &ranges[Slot::InputCurrentFrameNormalDepth]);
            rootParameters[Slot::InputCurrentFrameLinearDepthDerivative].InitAsDescriptorTable(1, &ranges[Slot::InputCurrentFrameLinearDepthDerivative]);
            rootParameters[Slot::InputReprojectedNormalDepth].InitAsDescriptorTable(1, &ranges[Slot::InputReprojectedNormalDepth]);
            rootParameters[Slot::InputTextureSpaceMotionVector].InitAsDescriptorTable(1, &ranges[Slot::InputTextureSpaceMotionVector]);
            rootParameters[Slot::InputCachedValue].InitAsDescriptorTable(1, &ranges[Slot::InputCachedValue]);
            rootParameters[Slot::InputCachedNormalDepth].InitAsDescriptorTable(1, &ranges[Slot::InputCachedNormalDepth]);
            rootParameters[Slot::InputCachedTrpp].InitAsDescriptorTable(1, &ranges[Slot::InputCachedTrpp]);
            rootParameters[Slot::InputCachedSquaredMeanValue].InitAsDescriptorTable(1, &ranges[Slot::InputCachedSquaredMeanValue]);
            rootParameters[Slot::InputCachedRayHitDistance].InitAsDescriptorTable(1, &ranges[Slot::InputCachedRayHitDistance]);
            rootParameters[Slot::OutputCacheTrpp].InitAsDescriptorTable(1, &ranges[Slot::OutputCacheTrpp]);
            rootParameters[Slot::OutputReprojectedCacheValues].InitAsDescriptorTable(1, &ranges[Slot::OutputReprojectedCacheValues]);
            rootParameters[Slot::OutputDebug1].InitAsDescriptorTable(1, &ranges[Slot::OutputDebug1]);
            rootParameters[Slot::OutputDebug2].InitAsDescriptorTable(1, &ranges[Slot::OutputDebug2]);
            rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);

            CD3DX12_STATIC_SAMPLER_DESC staticSamplers[] = {
                CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP) };

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters, ARRAYSIZE(staticSamplers), staticSamplers);
            SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_rootSignature, L"Compute root signature: TemporalSupersampling_ReverseReproject");
        }

        // Create compute pipeline state.
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
            descComputePSO.pRootSignature = m_rootSignature.Get();

            descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pTemporalSupersampling_ReverseReprojectCS), ARRAYSIZE(g_pTemporalSupersampling_ReverseReprojectCS));

            ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_pipelineStateObject)));
            m_pipelineStateObject->SetName(L"Pipeline state object: TemporalSupersampling_ReverseReproject");
        }

        // Create shader resources
        {
            m_CB.Create(device, frameCount * numCallsPerFrame, L"Constant Buffer: TemporalSupersampling_ReverseReproject");
        }
    }

    void TemporalSupersampling_ReverseReproject::Run(
        ID3D12GraphicsCommandList4* commandList,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* descriptorHeap,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCurrentFrameNormalDepthResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCurrentFrameLinearDepthDerivativeResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputReprojectedNormalDepthResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputTextureSpaceMotionVectorResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCachedValueResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCachedNormalDepthResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCachedTrppResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCachedSquaredMeanValue,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCachedRayHitDistanceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE outputReprojectedCacheTrppResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE outputReprojectedCacheValuesResourceHandle,
        float minSmoothingFactor,
        float depthTolerance,
        bool useDepthWeights,
        bool useNormalWeights,
        float floatEpsilonDepthTolerance,
        float depthDistanceBasedDepthTolerance,
        float depthSigma,
        bool useWorldSpaceDistance,
        bool usingBilateralDownsampledBuffers,
        bool perspectiveCorrectDepthInterpolation,
        GpuResource debugResources[2],
        const XMMATRIX& projectionToView,
        const XMMATRIX& prevProjectionToWorldWithCameraEyeAtOrigin,
        UINT maxTrpp,
        UINT numRaysToTraceSinceTemporalMovement)
    {
        using namespace RootSignature::TemporalSupersampling_ReverseReproject;
        using namespace DefaultComputeShaderParams;

        ScopedTimer _prof(L"TemporalSupersampling_ReverseReproject", commandList);

        m_CB->useDepthWeights = useDepthWeights;
        m_CB->useNormalWeights = useNormalWeights;
        m_CB->depthTolerance = depthTolerance;
        m_CB->textureDim = XMUINT2(width, height);
        m_CB->invTextureDim = XMFLOAT2(1.f / width, 1.f / height);
        m_CB->floatEpsilonDepthTolerance = floatEpsilonDepthTolerance;
        m_CB->depthDistanceBasedDepthTolerance = depthDistanceBasedDepthTolerance;
        m_CB->depthSigma = depthSigma;
        m_CB->projectionToView = XMMatrixTranspose(projectionToView);
        m_CB->prevProjectionToWorldWithCameraEyeAtOrigin = XMMatrixTranspose(prevProjectionToWorldWithCameraEyeAtOrigin);
        m_CB->useWorldSpaceDistance = useWorldSpaceDistance;
        m_CB->usingBilateralDownsampledBuffers = usingBilateralDownsampledBuffers;
        m_CB->perspectiveCorrectDepthInterpolation = perspectiveCorrectDepthInterpolation;
        m_CB->numRaysToTraceAfterTemporalAtMaxTrpp = numRaysToTraceSinceTemporalMovement;
        m_CB->maxTrpp = maxTrpp;
        m_CB->DepthNumMantissaBits = NumMantissaBitsInFloatFormat(16);
        m_CBinstanceID = (m_CBinstanceID + 1) % m_CB.NumInstances();
        m_CB.CopyStagingToGpu(m_CBinstanceID);

        // Set pipeline state.
        {
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetComputeRootDescriptorTable(Slot::InputCurrentFrameNormalDepth, inputCurrentFrameNormalDepthResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputCurrentFrameLinearDepthDerivative, inputCurrentFrameLinearDepthDerivativeResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputReprojectedNormalDepth, inputReprojectedNormalDepthResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputTextureSpaceMotionVector, inputTextureSpaceMotionVectorResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputCachedValue, inputCachedValueResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputCachedNormalDepth, inputCachedNormalDepthResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputCachedTrpp, inputCachedTrppResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputCachedSquaredMeanValue, inputCachedSquaredMeanValue);
            commandList->SetComputeRootDescriptorTable(Slot::InputCachedRayHitDistance, inputCachedRayHitDistanceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputCacheTrpp, outputReprojectedCacheTrppResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputReprojectedCacheValues, outputReprojectedCacheValuesResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputDebug1, debugResources[0].gpuDescriptorWriteAccess);
            commandList->SetComputeRootDescriptorTable(Slot::OutputDebug2, debugResources[1].gpuDescriptorWriteAccess);
            commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_CB.GpuVirtualAddress(m_CBinstanceID));
            commandList->SetPipelineState(m_pipelineStateObject.Get());
        }

        // Dispatch.
        XMUINT2 groupSize(CeilDivide(width, ThreadGroup::Width), CeilDivide(height, ThreadGroup::Height));
        commandList->Dispatch(groupSize.x, groupSize.y, 1);
    }


    namespace RootSignature {
        namespace TemporalSupersampling_BlendWithCurrentFrame {
            namespace Slot {
                enum Enum {
                    InputOutputValue = 0,
                    InputOutputTrpp,
                    InputOutputSquaredMeanValue,
                    InputOutputRayHitDistance,
                    OutputVariance,
                    OutputBlurStrength,
                    InputCurrentFrameValue,
                    InputCurrentFrameLocalMeanVariance,
                    InputCurrentFrameRayHitDistance,
                    InputReprojectedCacheValues,
                    OutputDebug1,
                    OutputDebug2,
                    ConstantBuffer,
                    Count
                };
            }
        }
    }

    void TemporalSupersampling_BlendWithCurrentFrame::Initialize(ID3D12Device5* device, UINT frameCount, UINT numCallsPerFrame)
    {
        // Create root signature.
        {
            using namespace RootSignature::TemporalSupersampling_BlendWithCurrentFrame;

            CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count];
            ranges[Slot::InputCurrentFrameValue].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            ranges[Slot::InputCurrentFrameLocalMeanVariance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
            ranges[Slot::InputCurrentFrameRayHitDistance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
            ranges[Slot::InputReprojectedCacheValues].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
            ranges[Slot::InputOutputValue].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            ranges[Slot::InputOutputTrpp].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
            ranges[Slot::InputOutputSquaredMeanValue].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2);
            ranges[Slot::InputOutputRayHitDistance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3);
            ranges[Slot::OutputVariance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4);
            ranges[Slot::OutputBlurStrength].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 5);
            ranges[Slot::OutputDebug1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 10);
            ranges[Slot::OutputDebug2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 11);

            CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
            rootParameters[Slot::InputCurrentFrameValue].InitAsDescriptorTable(1, &ranges[Slot::InputCurrentFrameValue]);
            rootParameters[Slot::InputCurrentFrameLocalMeanVariance].InitAsDescriptorTable(1, &ranges[Slot::InputCurrentFrameLocalMeanVariance]);
            rootParameters[Slot::InputCurrentFrameRayHitDistance].InitAsDescriptorTable(1, &ranges[Slot::InputCurrentFrameRayHitDistance]);
            rootParameters[Slot::InputReprojectedCacheValues].InitAsDescriptorTable(1, &ranges[Slot::InputReprojectedCacheValues]);
            rootParameters[Slot::InputOutputValue].InitAsDescriptorTable(1, &ranges[Slot::InputOutputValue]);
            rootParameters[Slot::InputOutputTrpp].InitAsDescriptorTable(1, &ranges[Slot::InputOutputTrpp]);
            rootParameters[Slot::InputOutputSquaredMeanValue].InitAsDescriptorTable(1, &ranges[Slot::InputOutputSquaredMeanValue]);
            rootParameters[Slot::InputOutputRayHitDistance].InitAsDescriptorTable(1, &ranges[Slot::InputOutputRayHitDistance]);
            rootParameters[Slot::OutputVariance].InitAsDescriptorTable(1, &ranges[Slot::OutputVariance]);
            rootParameters[Slot::OutputBlurStrength].InitAsDescriptorTable(1, &ranges[Slot::OutputBlurStrength]);
            rootParameters[Slot::OutputDebug1].InitAsDescriptorTable(1, &ranges[Slot::OutputDebug1]);
            rootParameters[Slot::OutputDebug2].InitAsDescriptorTable(1, &ranges[Slot::OutputDebug2]);
            rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_rootSignature, L"Compute root signature: TemporalSupersampling_BlendWithCurrentFrame");
        }

        // Create compute pipeline state.
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
            descComputePSO.pRootSignature = m_rootSignature.Get();

            descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pTemporalSupersampling_BlendWithCurrentFrameCS), ARRAYSIZE(g_pTemporalSupersampling_BlendWithCurrentFrameCS));

            ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_pipelineStateObject)));
            m_pipelineStateObject->SetName(L"Pipeline state object: TemporalSupersampling_BlendWithCurrentFrame");
        }

        // Create shader resources
        {
            m_CB.Create(device, frameCount * numCallsPerFrame, L"Constant Buffer: TemporalSupersampling_BlendWithCurrentFrame");
        }
    }

    void TemporalSupersampling_BlendWithCurrentFrame::Run(
        ID3D12GraphicsCommandList4* commandList,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* descriptorHeap,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCurrentFrameValueResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCurrentFrameLocalMeanVarianceResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputCurrentFrameRayHitDistanceResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputOutputValueResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputOutputTrppResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputOutputSquaredMeanValueResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputOutputRayHitDistanceResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputReprojectedCacheValuesResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE outputVarianceResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE outputBlurStrengthResourceHandle,
        float minSmoothingFactor,
        bool forceUseMinSmoothingFactor,
        bool clampCachedValues,
        float clampStdDevGamma,
        float clampMinStdDevTolerance,
        UINT minTrppToUseTemporalVariance,
        float clampDifferenceToTrppScale,
        GpuResource debugResources[2],
        UINT numFramesToDenoiseAfterLastTracedRay,
        UINT lowTrppBlurStrengthMaxTrpp,
        float lowTrppBlurStrengthDecayConstant,
        bool doCheckerboardSampling,
        bool checkerboardLoadEvenPixels)
    {
        using namespace RootSignature::TemporalSupersampling_BlendWithCurrentFrame;
        using namespace DefaultComputeShaderParams;

        ScopedTimer _prof(L"TemporalSupersampling_BlendWithCurrentFrame", commandList);

        m_CB->minSmoothingFactor = minSmoothingFactor;
        m_CB->forceUseMinSmoothingFactor = forceUseMinSmoothingFactor;
        m_CB->textureDim = XMUINT2(width, height);
        m_CB->invTextureDim = XMFLOAT2(1.f / width, 1.f / height);
        m_CB->clampCachedValues = clampCachedValues;
        m_CB->stdDevGamma = clampStdDevGamma;
        m_CB->minStdDevTolerance = clampMinStdDevTolerance;
        m_CB->minTrppToUseTemporalVariance = minTrppToUseTemporalVariance;
        m_CB->clampDifferenceToTrppScale = clampDifferenceToTrppScale;
        m_CB->numFramesToDenoiseAfterLastTracedRay = numFramesToDenoiseAfterLastTracedRay;
        m_CB->blurStrength_MaxTrpp = lowTrppBlurStrengthMaxTrpp;
        m_CB->blurDecayStrength = lowTrppBlurStrengthDecayConstant;
        m_CB->doCheckerboardSampling = doCheckerboardSampling;
        m_CB->areEvenPixelsActive = checkerboardLoadEvenPixels;
        m_CBinstanceID = (m_CBinstanceID + 1) % m_CB.NumInstances();
        m_CB.CopyStagingToGpu(m_CBinstanceID);

        // Set pipeline state.
        {
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetComputeRootDescriptorTable(Slot::InputCurrentFrameValue, inputCurrentFrameValueResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputCurrentFrameLocalMeanVariance, inputCurrentFrameLocalMeanVarianceResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputCurrentFrameRayHitDistance, inputCurrentFrameRayHitDistanceResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputOutputValue, inputOutputValueResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputOutputTrpp, inputOutputTrppResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputOutputSquaredMeanValue, inputOutputSquaredMeanValueResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputOutputRayHitDistance, inputOutputRayHitDistanceResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputReprojectedCacheValues, inputReprojectedCacheValuesResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputVariance, outputVarianceResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputBlurStrength, outputBlurStrengthResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputDebug1, debugResources[0].gpuDescriptorWriteAccess);
            commandList->SetComputeRootDescriptorTable(Slot::OutputDebug2, debugResources[1].gpuDescriptorWriteAccess);
            commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_CB.GpuVirtualAddress(m_CBinstanceID));
            commandList->SetPipelineState(m_pipelineStateObject.Get());
        }

        // Dispatch.
        XMUINT2 groupSize(CeilDivide(width, ThreadGroup::Width), CeilDivide(height, ThreadGroup::Height));
        commandList->Dispatch(groupSize.x, groupSize.y, 1);
    }


    namespace RootSignature {
        namespace SortRays {
            namespace Slot {
                enum Enum {
                    OutputSortedToSourceRayIndexOffset = 0,
                    Input,
                    OutputDebug,
                    ConstantBuffer,
                    Count
                };
            }
        }
    }

    void SortRays::Initialize(ID3D12Device5* device, UINT frameCount, UINT numCallsPerFrame)
    {
        // Create root signature.
        {
            using namespace RootSignature::SortRays;

            CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count];
            ranges[Slot::Input].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);  // 1 input texture
            ranges[Slot::OutputSortedToSourceRayIndexOffset].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);  // 1 output texture
            ranges[Slot::OutputDebug].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2);  // 1 output texture

            CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
            rootParameters[Slot::Input].InitAsDescriptorTable(1, &ranges[Slot::Input]);
            rootParameters[Slot::OutputSortedToSourceRayIndexOffset].InitAsDescriptorTable(1, &ranges[Slot::OutputSortedToSourceRayIndexOffset]);
            rootParameters[Slot::OutputDebug].InitAsDescriptorTable(1, &ranges[Slot::OutputDebug]);
            rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_rootSignature, L"Compute root signature: Sort Rays");
        }

        // Create compute pipeline state.
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
            descComputePSO.pRootSignature = m_rootSignature.Get();
            descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pCountingSort_SortRays_64x128rayGroupCS), ARRAYSIZE(g_pCountingSort_SortRays_64x128rayGroupCS));

            ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_pipelineStateObject)));
            m_pipelineStateObject->SetName(L"Pipeline state object: Sort Rays");
        }

        // Create shader resources
        {
            m_CB.Create(device, frameCount * numCallsPerFrame, L"Constant Buffer: Sort Rays");
        }
    }

    // width, height - dimensions of the input resource.
    void SortRays::Run(
        ID3D12GraphicsCommandList4* commandList,
        float binDepthSize,
        UINT width,
        UINT height,
        bool useOctahedralRayDirectionQuantization,
        ID3D12DescriptorHeap* descriptorHeap,
        D3D12_GPU_DESCRIPTOR_HANDLE inputRayDirectionOriginDepthResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE outputSortedToSourceRayIndexOffsetResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE outputDebugResourceHandle)
    {
        using namespace RootSignature::SortRays;
        using namespace SortRays;

        ScopedTimer _prof(L"Sort Rays", commandList);

        m_CB->dim = XMUINT2(width, height);
        m_CB->useOctahedralRayDirectionQuantization = useOctahedralRayDirectionQuantization;
        m_CB->binDepthSize = binDepthSize;
        m_CBinstanceID = (m_CBinstanceID + 1) % m_CB.NumInstances();
        m_CB.CopyStagingToGpu(m_CBinstanceID);

        // Set pipeline state.
        {
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetComputeRootDescriptorTable(Slot::Input, inputRayDirectionOriginDepthResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputSortedToSourceRayIndexOffset, outputSortedToSourceRayIndexOffsetResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputDebug, outputDebugResourceHandle);
            commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_CB.GpuVirtualAddress(m_CBinstanceID));
            commandList->SetPipelineState(m_pipelineStateObject.Get());
        }

        // Dispatch.
        XMUINT2 groupSize(CeilDivide(width, RayGroup::Width), CeilDivide(height, RayGroup::Height));
        commandList->Dispatch(groupSize.x, groupSize.y, 1);
    }


    namespace RootSignature {
        namespace AORayGenerator {
            namespace Slot {
                enum Enum {
                    OutputRayDirectionOriginDepth = 0,
                    InputRayOriginSurfaceNormalDepth,
                    InputRayOriginPosition,
                    InputAlignedHemisphereSamples,
                    ConstantBuffer,
                    Count
                };
            }
        }
    }

    void AORayGenerator::Initialize(ID3D12Device5* device, UINT frameCount, UINT numCallsPerFrame)
    {
        // Create root signature.
        {
            using namespace RootSignature::AORayGenerator;

            CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count];
            ranges[Slot::InputRayOriginSurfaceNormalDepth].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            ranges[Slot::InputRayOriginPosition].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
            ranges[Slot::OutputRayDirectionOriginDepth].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

            CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
            rootParameters[Slot::InputRayOriginSurfaceNormalDepth].InitAsDescriptorTable(1, &ranges[Slot::InputRayOriginSurfaceNormalDepth]);
            rootParameters[Slot::InputRayOriginPosition].InitAsDescriptorTable(1, &ranges[Slot::InputRayOriginPosition]);
            rootParameters[Slot::OutputRayDirectionOriginDepth].InitAsDescriptorTable(1, &ranges[Slot::OutputRayDirectionOriginDepth]);
            rootParameters[Slot::InputAlignedHemisphereSamples].InitAsShaderResourceView(3);
            rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);

            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_rootSignature, L"Compute root signature: AO Ray Generator Rays");
        }

        // Create compute pipeline state.
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
            descComputePSO.pRootSignature = m_rootSignature.Get();
            descComputePSO.CS = CD3DX12_SHADER_BYTECODE(static_cast<const void*>(g_pAORayGenCS), ARRAYSIZE(g_pAORayGenCS));

            ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_pipelineStateObject)));
            m_pipelineStateObject->SetName(L"Pipeline state object: AO Ray Generator");
        }

        // Create shader resources
        {
            m_CB.Create(device, frameCount * numCallsPerFrame, L"Constant Buffer: AO Ray Generator");
        }
    }

    // width, height - dimensions of the input resource.
    void AORayGenerator::Run(
        ID3D12GraphicsCommandList4* commandList,
        UINT width,
        UINT height,
        UINT seed,
        UINT numSamplesPerSet,
        UINT numSampleSets,
        UINT numPixelsPerDimPerSet,
        bool doCheckerboardRayGeneration,
        bool checkerboardGenerateRaysForEvenPixels,
        ID3D12DescriptorHeap* descriptorHeap,
        D3D12_GPU_DESCRIPTOR_HANDLE inputRayOriginSurfaceNormalDepthResourceHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE inputRayOriginPositionResourceHandle,
        D3D12_GPU_VIRTUAL_ADDRESS inputAlignedHemisphereSamplesBufferAddress,
        D3D12_GPU_DESCRIPTOR_HANDLE outputRayDirectionOriginDepthResourceHandle)
    {
        using namespace RootSignature::AORayGenerator;
        using namespace DefaultComputeShaderParams;

        ScopedTimer _prof(L"AO Ray Generator", commandList);

        m_CB->textureDim = XMUINT2(width, height);
        m_CB->seed = seed;
        m_CB->numSamplesPerSet = numSamplesPerSet;
        m_CB->numPixelsPerDimPerSet = numPixelsPerDimPerSet;
        m_CB->numSampleSets = numSampleSets;
        m_CB->doCheckerboardRayGeneration = doCheckerboardRayGeneration;
        m_CB->checkerboardGenerateRaysForEvenPixels = checkerboardGenerateRaysForEvenPixels;

        m_CBinstanceID = (m_CBinstanceID + 1) % m_CB.NumInstances();
        m_CB.CopyStagingToGpu(m_CBinstanceID);

        // Set pipeline state.
        {
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetComputeRootDescriptorTable(Slot::InputRayOriginSurfaceNormalDepth, inputRayOriginSurfaceNormalDepthResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::InputRayOriginPosition, inputRayOriginPositionResourceHandle);
            commandList->SetComputeRootDescriptorTable(Slot::OutputRayDirectionOriginDepth, outputRayDirectionOriginDepthResourceHandle);
            commandList->SetComputeRootShaderResourceView(Slot::InputAlignedHemisphereSamples, inputAlignedHemisphereSamplesBufferAddress);
            commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_CB.GpuVirtualAddress(m_CBinstanceID));
            commandList->SetPipelineState(m_pipelineStateObject.Get());
        }

        // Dispatch.
        XMUINT2 groupSize(CeilDivide(width, ThreadGroup::Width), CeilDivide(height, ThreadGroup::Height));
        commandList->Dispatch(groupSize.x, groupSize.y, 1);
    }
}