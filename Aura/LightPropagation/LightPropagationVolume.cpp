/*
 * Copyright © 2018-2021 Confetti Interactive Inc.
 *
 * This is a part of Aura.
 * 
 * This file(code) is licensed under a 
 * Creative Commons Attribution-NonCommercial 4.0 International License 
 *
 *   (https://creativecommons.org/licenses/by-nc/4.0/legalcode) 
 *
 * Based on a work at https://github.com/ConfettiFX/The-Forge.
 * You may not use the material for commercial purposes.
 *
*/

#include "LightPropagationVolume.h"
#include "../Math/AuraVector.h"

using aura::float4;

#ifdef XBOX
#include <DirectXPackedVector.h>
#endif

#define NO_FSL_DEFINITIONS
#include "../Shaders/FSL/lpvCommon.h"
#include "../Shaders/FSL/lightPropagation.h"
#include "../Shaders/FSL/lpvSHMaths.h"

#include "../Interfaces/IAuraMemoryManager.h"

#include "LightPropagationGrid.h"


static const int QuadVertexCount = 6;

namespace aura
{
	float getCellSize(LightPropagationCascade* pCascade)
	{
		return pCascade->mGridSpan / GridRes;
	}

	float getSideHalf(LightPropagationCascade* pCascade)
	{
		return pCascade->mGridSpan / 2.0f;
	}

	Box getGridBounds(LightPropagationCascade* pCascade, const mat4 &worldToLocal)
	{
		mat4 gridToLocal = worldToLocal * pCascade->mInjectState.mGridToWorld;

		vec3 posMin = (gridToLocal * float4(0.0f, 0.0f, 0.0f, 1.0f)).xyz();
		vec3 posMax = posMin;

		for (int i = 1; i < 8; ++i)
		{
			vec3 localPos = (gridToLocal * float4(
				(i & 1) ? 1.0f : 0.0f,
				(i & 2) ? 1.0f : 0.0f,
				(i & 4) ? 1.0f : 0.0f,
				1.0f)).xyz();

#ifdef XBOX
			// Tim: XBOX doesn't like the min/max in our math library, or they are not
			// getting called correctly when running in a PROFILE/RELEASE build
			posMin = vec3(
				min(posMin.x, localPos.x),
				min(posMin.y, localPos.y),
				min(posMin.z, localPos.z));
			posMax = vec3(
				max(posMax.x, localPos.x),
				max(posMax.y, localPos.y),
				max(posMax.z, localPos.z));
#else
			posMin = min(posMin, localPos);
			posMax = max(posMax, localPos);
#endif
		}

		return { posMin, posMax };
	}

	void setGridCenter(LightPropagationCascade* pCascade, vec3 gridCenter)
	{
		const float cellSize = getCellSize(pCascade);
		const float	sideHalf = getSideHalf(pCascade);
		vec3 smoothGridCenter = gridCenter;

		//	Snap to grid
		gridCenter /= cellSize;
		gridCenter.x = roundf(gridCenter.x);
		gridCenter.y = roundf(gridCenter.y);
		gridCenter.z = roundf(gridCenter.z);
		gridCenter *= cellSize;

		pCascade->mInjectState.mGridToWorld = translate(gridCenter) * scale(sideHalf, sideHalf, sideHalf) *
			(translate(-1.0f, -1.0f, -1.0f) * scale(2.0f, 2.0f, 2.0f));
		pCascade->mInjectState.mWorldToGrid = !pCascade->mInjectState.mGridToWorld;

		//	Igor: easy to calculate. Store just to make code cleaner.
		pCascade->mInjectState.mWorldToGridScale = vec3(pCascade->mInjectState.mWorldToGrid.rows[0].x, pCascade->mInjectState.mWorldToGrid.rows[1].y, pCascade->mInjectState.mWorldToGrid.rows[2].z);
		pCascade->mInjectState.mWorldToGridTranslate = vec3(pCascade->mInjectState.mWorldToGrid.rows[0].w, pCascade->mInjectState.mWorldToGrid.rows[1].w, pCascade->mInjectState.mWorldToGrid.rows[2].w);

		//	Use this offset to calculate non-snapped grid position of the object.
		//	Is used to smoothly blend in/out light on the border of the grid
		//	and to smoothly blend grid when light grid is applied.
		pCascade->mInjectState.mSmoothTCOffset = (pCascade->mInjectState.mWorldToGrid * float4(gridCenter, 1.0f)
			- pCascade->mInjectState.mWorldToGrid * float4(smoothGridCenter, 1.0f)).xyz();
	}

	void beginFrame(LightPropagationCascade* pCascade, const vec3 &camPos, const vec3 &camDir)
	{
		const float cellSize = getCellSize(pCascade);
		const float	sideHalf = getSideHalf(pCascade);

		//	Offset grid. We want grid to be only in front of the camera to maximize grid usage.
		//	Still we want the camera to be inside of the grid
		float maxOffset = max(fabsf(camDir.x), fabsf(camDir.y));
		maxOffset = max(maxOffset, fabsf(camDir.z));
		vec3 offset = camDir / maxOffset;
		//offset *= (sideHalf-8*cellSize);
		//	Leave some cells behind to allow light behind the camera to propagate forward
		offset *= (sideHalf - 4 * cellSize);
		vec3 gridCenter = camPos + offset;
		if ((!pCascade->mFlags) & CASCADE_NOT_MOVING)
			setGridCenter(pCascade, gridCenter);

		pCascade->mOccludersInjected = false;
	}

	/************************************************************************/
	// Aura Implementation
	/************************************************************************/
	void initAura(Renderer* pRenderer, PipelineCache* pCache, uint32_t rtWidth, uint32_t rtHeight, LightPropagationVolumeParams params, uint32_t inFlightFrameCount, uint32_t options, TinyImageFormat visualizeFormat, TinyImageFormat visualizeDepthFormat, SampleCount sampleCount, uint32_t sampleQuality, uint32_t cascadeCount, LightPropagationCascadeDesc* pCascades, Aura** ppAura)
	{
		Aura* pAura = (Aura*)aura::alloc(sizeof(*pAura));
		memset(pAura, 0, sizeof(*pAura));

		pAura->mParams = params;

#ifdef ENABLE_CPU_PROPAGATION
		pAura->bUseCPUPropagationPreviousFrame = pAura->mParams.bUseCPUPropagation;
		pAura->mInFlightFrameCount = inFlightFrameCount;
#endif

		pAura->mOptions = options;
		pAura->mCascadeCount = cascadeCount;

		/************************************************************************/
		// Load render targets
		/************************************************************************/
		pAura->pCascades = (LightPropagationCascade**)aura::alloc(pAura->mCascadeCount * sizeof(*pAura->pCascades));
		for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
		{
			addLightPropagationCascade(pRenderer, pCascades[i].mGridSpan, pCascades[i].mGridIntensity, pCascades[i].mFlags, &pAura->pCascades[i]);
		}

		for (uint32_t i = 0; i < 6; ++i)
			addLightPropagationGrid(pRenderer, &pAura->pWorkingGrids[i], "LPV Working Grid RT");
		/************************************************************************/
		// CPU contexts
		/************************************************************************/
#ifdef ENABLE_CPU_PROPAGATION
		loadCPUPropagationResources(pRenderer, pAura);
#endif
		/************************************************************************/
		// Default settings
		/************************************************************************/
#ifdef ENABLE_CPU_PROPAGATION
		pAura->mCPUPropagationCurrentContext = -2;
#endif
		pAura->mGPUPropagationCurrentGrid = 0;
		pAura->mParams.bDebugLight = false;
		pAura->mParams.bDebugOccluder = false;
		/************************************************************************/
		// Samplers
		/************************************************************************/
		SamplerDesc pointDesc = {
			FILTER_NEAREST, FILTER_NEAREST, MIPMAP_MODE_NEAREST,
			ADDRESS_MODE_CLAMP_TO_BORDER, ADDRESS_MODE_CLAMP_TO_BORDER, ADDRESS_MODE_CLAMP_TO_BORDER
		};
		addSampler(pRenderer, &pointDesc, &pAura->pSamplerPointBorder);
		/************************************************************************/
		// Load Shaders
		/************************************************************************/
		ShaderMacro macros[5] = {};
		uint32_t count = 0;
		getShaderMacros(pAura, macros, &count);

		ShaderLoadDesc injectRSMLightDesc = {};
		ShaderLoadDesc lightPropagate1Desc = {};
		ShaderLoadDesc lightPropagateNDesc = {};
		ShaderLoadDesc lightCopyDesc = {};
		
		injectRSMLightDesc.mStages[0] = { "lpvInjectRSMLight.vert", macros, count };
		injectRSMLightDesc.mStages[1] = { "lpvInjectRSMLight.frag", macros, count };

#if USE_COMPUTE_SHADERS
		lightPropagate1Desc.mStages[0] = { "lpvLightPropagate1.comp", macros, count };
		lightPropagateNDesc.mStages[0] = { "lpvLightPropagateN.comp", macros, count };
		lightCopyDesc.mStages[0] = { "lpvLightCopy.comp", macros, count };
#else
		lightPropagate1Desc.mStages[0] = { "lpvLightPropagate1.vert", macros, count };
		lightPropagate1Desc.mStages[1] = { "lpvLightPropagate1.frag", macros, count };
		
		lightPropagateNDesc.mStages[0] = { "lpvLightPropagateN.vert", macros, count };
		lightPropagateNDesc.mStages[1] = { "lpvLightPropagateN.frag", macros, count };

		lightCopyDesc.mStages[0] = { "lpvLightCopy.vert", macros, count };
		lightCopyDesc.mStages[1] = { "lpvLightCopy.frag", macros, count };
#endif // USE_COMPUTE_SHADERS

		addShader(pRenderer, &injectRSMLightDesc, &pAura->pShaderInjectRSMLight);
		addShader(pRenderer, &lightPropagate1Desc, &pAura->pShaderLightPropagate1[0]);
		addShader(pRenderer, &lightPropagateNDesc, &pAura->pShaderLightPropagateN[0]);
		addShader(pRenderer, &lightCopyDesc, &pAura->pShaderLightCopy);

		ShaderLoadDesc visualizeLPVDesc = {};
		visualizeLPVDesc.mStages[0] = { "lpvVisualize.vert", macros, count };
		visualizeLPVDesc.mStages[1] = { "lpvVisualize.frag", macros, count };
		addShader(pRenderer, &visualizeLPVDesc, &pAura->pShaderLPVVisualize);

		/************************************************************************/
		// Add root signatures
		/************************************************************************/
		const char* pStaticSamplerNames[] = {
			"pointBorder",
		};
		Sampler* pStaticSamplers[] = {
			pAura->pSamplerPointBorder,
		};

		RootSignatureDesc injectRSMRootDesc = { &pAura->pShaderInjectRSMLight, 1 };
		injectRSMRootDesc.mStaticSamplerCount = 1;
		injectRSMRootDesc.mMaxBindlessTextures = 9;
		injectRSMRootDesc.ppStaticSamplerNames = pStaticSamplerNames;
		injectRSMRootDesc.ppStaticSamplers = pStaticSamplers;
		addRootSignature(pRenderer, &injectRSMRootDesc, &pAura->pRootSignatureInjectRSMLight);

		RootSignatureDesc propagate1RootDesc = { &pAura->pShaderLightPropagate1[0], 1 };
		propagate1RootDesc.mStaticSamplerCount = 1;
		propagate1RootDesc.mMaxBindlessTextures = 9;
		propagate1RootDesc.ppStaticSamplerNames = pStaticSamplerNames;
		propagate1RootDesc.ppStaticSamplers = pStaticSamplers;
		addRootSignature(pRenderer, &propagate1RootDesc, &pAura->pRootSignatureLightPropagate1);

		RootSignatureDesc propagateNRootDesc = { &pAura->pShaderLightPropagateN[0], 1 };
		propagateNRootDesc.mStaticSamplerCount = 1;
		propagateNRootDesc.mMaxBindlessTextures = 9;
		propagateNRootDesc.ppStaticSamplerNames = pStaticSamplerNames;
		propagateNRootDesc.ppStaticSamplers = pStaticSamplers;
		addRootSignature(pRenderer, &propagateNRootDesc, &pAura->pRootSignatureLightPropagateN);

		RootSignatureDesc copyRootDesc = { &pAura->pShaderLightCopy, 1 };
		copyRootDesc.mStaticSamplerCount = 1;
		copyRootDesc.mMaxBindlessTextures = 9;
		copyRootDesc.ppStaticSamplerNames = pStaticSamplerNames;
		copyRootDesc.ppStaticSamplers = pStaticSamplers;
		addRootSignature(pRenderer, &copyRootDesc, &pAura->pRootSignatureLightCopy);

		RootSignatureDesc visualizeLPVRootDesc = { &pAura->pShaderLPVVisualize, 1 };
		visualizeLPVRootDesc.mStaticSamplerCount = 1;
		visualizeLPVRootDesc.mMaxBindlessTextures = 9;
		visualizeLPVRootDesc.ppStaticSamplerNames = pStaticSamplerNames;
		visualizeLPVRootDesc.ppStaticSamplers = pStaticSamplers;
		addRootSignature(pRenderer, &visualizeLPVRootDesc, &pAura->pRootSignatureVisualizeLPV);

		/************************************************************************/
		// Add Descriptor Sets
		/************************************************************************/
		DescriptorSetDesc setDesc = { pAura->pRootSignatureInjectRSMLight, DESCRIPTOR_UPDATE_FREQ_NONE, pAura->mCascadeCount * MAX_FRAMES };
		addDescriptorSet(pRenderer, &setDesc, &pAura->pDescriptorSetInjectRSMLight);
		setDesc = { pAura->pRootSignatureLightPropagate1, DESCRIPTOR_UPDATE_FREQ_NONE, pAura->mCascadeCount };
		addDescriptorSet(pRenderer, &setDesc, &pAura->pDescriptorSetLightPropagate1);
		setDesc = { pAura->pRootSignatureLightPropagateN, DESCRIPTOR_UPDATE_FREQ_NONE, pAura->mCascadeCount * 2 };
		addDescriptorSet(pRenderer, &setDesc, &pAura->pDescriptorSetLightPropagateN);
#if USE_COMPUTE_SHADERS
		setDesc = { pAura->pRootSignatureLightCopy, DESCRIPTOR_UPDATE_FREQ_NONE, pAura->mCascadeCount };
#else
		setDesc = { pAura->pRootSignatureLightCopy, DESCRIPTOR_UPDATE_FREQ_NONE, pAura->mCascadeCount * 2 };
#endif
		addDescriptorSet(pRenderer, &setDesc, &pAura->pDescriptorSetLightCopy);

		setDesc = { pAura->pRootSignatureVisualizeLPV, DESCRIPTOR_UPDATE_FREQ_NONE, MAX_FRAMES };
		addDescriptorSet(pRenderer, &setDesc, &pAura->pDescriptorSetVisualizeLPV);
		/************************************************************************/
		// Add pipelines
		/************************************************************************/
		// States
		BlendStateDesc blendStateAddDesc = {};
		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
		{
			blendStateAddDesc.mSrcAlphaFactors[i] = BC_ONE;
			blendStateAddDesc.mDstAlphaFactors[i] = BC_ONE;
			blendStateAddDesc.mSrcFactors[i] = BC_ONE;
			blendStateAddDesc.mDstFactors[i] = BC_ONE;
			blendStateAddDesc.mMasks[i] = ALL;
		}
		blendStateAddDesc.mRenderTargetMask = BLEND_STATE_TARGET_0 | BLEND_STATE_TARGET_1 | BLEND_STATE_TARGET_2;
		blendStateAddDesc.mIndependentBlend = false;

		BlendStateDesc blendStateMaxDesc = {};
		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
		{
			blendStateMaxDesc.mSrcAlphaFactors[i] = BC_ONE;
			blendStateMaxDesc.mDstAlphaFactors[i] = BC_ONE;
			blendStateMaxDesc.mSrcFactors[i] = BC_ONE;
			blendStateMaxDesc.mDstFactors[i] = BC_ONE;
			blendStateMaxDesc.mMasks[i] = ALL;
			blendStateMaxDesc.mBlendAlphaModes[i] = BM_MAX;
			blendStateMaxDesc.mBlendModes[i] = BM_MAX;
		}
		blendStateMaxDesc.mRenderTargetMask = BLEND_STATE_TARGET_0 | BLEND_STATE_TARGET_1 | BLEND_STATE_TARGET_2;
		blendStateMaxDesc.mIndependentBlend = false;

		BlendStateDesc blendStateAlphaDesc = {};
		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
		{
			blendStateAlphaDesc.mSrcAlphaFactors[i] = BC_SRC_ALPHA;
			blendStateAlphaDesc.mDstAlphaFactors[i] = BC_DST_ALPHA;
			blendStateAlphaDesc.mSrcFactors[i] = BC_SRC_ALPHA;
			blendStateAlphaDesc.mDstFactors[i] = BC_ONE_MINUS_SRC_ALPHA;
			blendStateAlphaDesc.mMasks[i] = ALL;
			blendStateAlphaDesc.mBlendAlphaModes[i] = BM_MAX;
			blendStateAlphaDesc.mBlendModes[i] = BM_ADD;
		}
		blendStateAlphaDesc.mRenderTargetMask = BLEND_STATE_TARGET_0 | BLEND_STATE_TARGET_1 | BLEND_STATE_TARGET_2;
		blendStateAlphaDesc.mIndependentBlend = false;

#ifdef	PROPAGATE_ACCUMULATE_ONE_PASS
		BlendStateDesc blendStateAdd345Desc = {};
		blendStateAdd345Desc.mRenderTargetMask = BLEND_STATE_TARGET_3 | BLEND_STATE_TARGET_4 | BLEND_STATE_TARGET_5;
#else	//	PROPAGATE_ACCUMULATE_ONE_PASS
#endif	//	PROPAGATE_ACCUMULATE_ONE_PASS

		RasterizerStateDesc rasterizerStateDesc = {};
		rasterizerStateDesc.mCullMode = CULL_MODE_NONE;

		DepthStateDesc depthStateDisableDesc = {};

		// TODO - This should be informed from the engine side. At least the compare function. 
		DepthStateDesc depthStateDesc = {};
		depthStateDesc.mDepthTest = true;
		depthStateDesc.mDepthWrite = true;
		depthStateDesc.mDepthFunc = CMP_LEQUAL;


		TinyImageFormat gridRTDesc[NUM_GRIDS_PER_CASCADE] = {};
		bool srgbValues[NUM_GRIDS_PER_CASCADE] = {};
		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
		{
			srgbValues[i] = false;
			gridRTDesc[i] = TinyImageFormat_R16G16B16A16_SFLOAT;
		}

		PipelineDesc graphicsPipelineDesc = {};
		graphicsPipelineDesc.pCache = pCache;
		graphicsPipelineDesc.mType = PIPELINE_TYPE_GRAPHICS;
		PipelineDesc computePipelineDesc = {};
		computePipelineDesc.pCache = pCache;
		computePipelineDesc.mType = PIPELINE_TYPE_COMPUTE;

		GraphicsPipelineDesc injectRSMPipelineDesc = {};
		injectRSMPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_POINT_LIST;
		injectRSMPipelineDesc.mRenderTargetCount = NUM_GRIDS_PER_CASCADE;
		injectRSMPipelineDesc.pBlendState = &blendStateAddDesc;
		injectRSMPipelineDesc.pDepthState = &depthStateDisableDesc;
		injectRSMPipelineDesc.pColorFormats = gridRTDesc;
		injectRSMPipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		injectRSMPipelineDesc.mSampleQuality = 0;
		injectRSMPipelineDesc.pRasterizerState = &rasterizerStateDesc;
		injectRSMPipelineDesc.pRootSignature = pAura->pRootSignatureInjectRSMLight;
		injectRSMPipelineDesc.pShaderProgram = pAura->pShaderInjectRSMLight;
		graphicsPipelineDesc.mGraphicsDesc = injectRSMPipelineDesc;
		addPipeline(pRenderer, &graphicsPipelineDesc, &pAura->pPipelineInjectRSMLight);

#if USE_COMPUTE_SHADERS
		ComputePipelineDesc propagatePipelineDesc = {};
		propagatePipelineDesc.pRootSignature = pAura->pRootSignatureLightPropagate1;
		propagatePipelineDesc.pShaderProgram = pAura->pShaderLightPropagate1[0];
		computePipelineDesc.mComputeDesc = propagatePipelineDesc;
		addPipeline(pRenderer, &computePipelineDesc, &pAura->pPipelineLightPropagate1[0]);

		propagatePipelineDesc.pRootSignature = pAura->pRootSignatureLightPropagateN;
		propagatePipelineDesc.pShaderProgram = pAura->pShaderLightPropagateN[0];
		computePipelineDesc.mComputeDesc = propagatePipelineDesc;
		addPipeline(pRenderer, &computePipelineDesc, &pAura->pPipelineLightPropagateN[0]);

		ComputePipelineDesc lightCopyPipelineDesc = {};
		lightCopyPipelineDesc.pRootSignature = pAura->pRootSignatureLightCopy;
		lightCopyPipelineDesc.pShaderProgram = pAura->pShaderLightCopy;
		computePipelineDesc.mComputeDesc = lightCopyPipelineDesc;
		addPipeline(pRenderer, &computePipelineDesc, &pAura->pPipelineLightCopy);
#else
		GraphicsPipelineDesc propagatePipelineDesc = {};
		propagatePipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		propagatePipelineDesc.mRenderTargetCount = NUM_GRIDS_PER_CASCADE;
		propagatePipelineDesc.pDepthState = &depthStateDisableDesc;
		propagatePipelineDesc.pColorFormats = gridRTDesc;
		propagatePipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		propagatePipelineDesc.mSampleQuality = 0;
		propagatePipelineDesc.pRasterizerState = &rasterizerStateDesc;
		propagatePipelineDesc.pRootSignature = pAura->pRootSignatureLightPropagate1;
		propagatePipelineDesc.pShaderProgram = pAura->pShaderLightPropagate1[0];
		graphicsPipelineDesc.mGraphicsDesc = propagatePipelineDesc;
		addPipeline(pRenderer, &graphicsPipelineDesc, &pAura->pPipelineLightPropagate1[0]);

		propagatePipelineDesc.pRootSignature = pAura->pRootSignatureLightPropagateN;
		propagatePipelineDesc.pShaderProgram = pAura->pShaderLightPropagateN[0];
		graphicsPipelineDesc.mGraphicsDesc = propagatePipelineDesc;
		addPipeline(pRenderer, &graphicsPipelineDesc, &pAura->pPipelineLightPropagateN[0]);

		GraphicsPipelineDesc lightCopyPipelineDesc = {};
		lightCopyPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		lightCopyPipelineDesc.mRenderTargetCount = NUM_GRIDS_PER_CASCADE;
		lightCopyPipelineDesc.pBlendState = &blendStateAddDesc;
		lightCopyPipelineDesc.pDepthState = &depthStateDisableDesc;
		lightCopyPipelineDesc.pColorFormats = gridRTDesc;
		lightCopyPipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		lightCopyPipelineDesc.mSampleQuality = 0;
		lightCopyPipelineDesc.pRasterizerState = &rasterizerStateDesc;
		lightCopyPipelineDesc.pRootSignature = pAura->pRootSignatureLightCopy;
		lightCopyPipelineDesc.pShaderProgram = pAura->pShaderLightCopy;
		graphicsPipelineDesc.mGraphicsDesc = lightCopyPipelineDesc;
		addPipeline(pRenderer, &graphicsPipelineDesc, &pAura->pPipelineLightCopy);
#endif // USE_COMPUTE_SHADERS

		TinyImageFormat visualizeFormats[1] = { visualizeFormat };
		GraphicsPipelineDesc visualizeLPVPipelineDesc = {};
		visualizeLPVPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		visualizeLPVPipelineDesc.mRenderTargetCount = 1;
		visualizeLPVPipelineDesc.pBlendState = &blendStateAlphaDesc;
		visualizeLPVPipelineDesc.pDepthState = &depthStateDesc;
		visualizeLPVPipelineDesc.mDepthStencilFormat = visualizeDepthFormat;
		visualizeLPVPipelineDesc.pColorFormats = visualizeFormats;
		visualizeLPVPipelineDesc.mSampleCount = sampleCount;
		visualizeLPVPipelineDesc.mSampleQuality = sampleQuality;
		visualizeLPVPipelineDesc.pRasterizerState = &rasterizerStateDesc;
		visualizeLPVPipelineDesc.pRootSignature = pAura->pRootSignatureVisualizeLPV;
		visualizeLPVPipelineDesc.pShaderProgram = pAura->pShaderLPVVisualize;
		graphicsPipelineDesc.mGraphicsDesc = visualizeLPVPipelineDesc;
		addPipeline(pRenderer, &graphicsPipelineDesc, &pAura->pPipelineVisualizeLPV);
		/************************************************************************/
		// Add uniform buffers
		/************************************************************************/
		for (uint32_t i = 0; i < MAX_FRAMES; ++i)
		{
			pAura->pUniformBufferInjectRSM[i] = (Buffer**)aura::alloc(pAura->mCascadeCount * sizeof(*pAura->pUniformBufferInjectRSM[i]));

			for (uint32_t j = 0; j < pAura->mCascadeCount; ++j)
			{
				addUniformBuffer(pRenderer, sizeof(LightInjectionData), &pAura->pUniformBufferInjectRSM[i][j]);
			}

			addUniformBuffer(pRenderer, sizeof(VisualizationData), &pAura->pUniformBufferVisualizationData[i]);
		}
		/************************************************************************/
		/************************************************************************/
		for (uint32_t cascade = 0; cascade < pAura->mCascadeCount; ++cascade)
		{
			Texture* pTex[NUM_GRIDS_PER_CASCADE] = {};
			Texture* pWorkingTex[NUM_GRIDS_PER_CASCADE * 2] = {};

			for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
				getTextureFromRenderTarget(pAura->pCascades[cascade]->pLightGrids[i], &pTex[i]);
			for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE * 2; ++i)
				getTextureFromRenderTarget(pAura->pWorkingGrids[i], &pWorkingTex[i]);

			// Light propagate 1
			{
#if USE_COMPUTE_SHADERS
				DescriptorData params[4] = {};
				params[0].pName = "LPVGrid";
				params[0].ppTextures = &pTex[0];
				params[0].mCount = pAura->mCascadeCount;
				params[1].pName = "workCoeffs0";
				params[1].ppTextures = &pWorkingTex[0];
				params[2].pName = "workCoeffs1";
				params[2].ppTextures = &pWorkingTex[1];
				params[3].pName = "workCoeffs2";
				params[3].ppTextures = &pWorkingTex[2];
				updateDescriptorSet(pRenderer, cascade, pAura->pDescriptorSetLightPropagate1, 4, params);
#else
				DescriptorData params[1] = {};
				params[0].pName = "LPVGrid";
				params[0].ppTextures = &pTex[0];
				params[0].mCount = pAura->mCascadeCount;
				updateDescriptorSet(pRenderer, cascade, pAura->pDescriptorSetLightPropagate1, 1, params);
#endif
			}
			// Light copy
			{
#if USE_COMPUTE_SHADERS
				DescriptorData params[6] = {};
				params[0].pName = "workCoeffs0";
				params[0].ppTextures = &pWorkingTex[0];
				params[1].pName = "workCoeffs1";
				params[1].ppTextures = &pWorkingTex[1];
				params[2].pName = "workCoeffs2";
				params[2].ppTextures = &pWorkingTex[2];
				params[3].pName = "RWGrid0";
				params[3].ppTextures = &pTex[0];
				params[4].pName = "RWGrid1";
				params[4].ppTextures = &pTex[1];
				params[5].pName = "RWGrid2";
				params[5].ppTextures = &pTex[2];
				updateDescriptorSet(pRenderer, cascade, pAura->pDescriptorSetLightCopy, 6, params);
#else
				DescriptorData params[3] = {};
				params[0].pName = "LPVGrid";
				params[0].ppTextures = &pWorkingTex[0];
				params[0].mCount = pAura->mCascadeCount;
				updateDescriptorSet(pRenderer, cascade * 2 + 0, pAura->pDescriptorSetLightCopy, 1, params);

				params[0].ppTextures = &pWorkingTex[3];
				params[1].ppTextures = &pWorkingTex[4];
				params[2].ppTextures = &pWorkingTex[5];
				updateDescriptorSet(pRenderer, cascade * 2 + 1, pAura->pDescriptorSetLightCopy, 1, params);
#endif
			}
			// Light propagate N
			for (uint32_t i = 0; i < 2; ++i)
			{
#if USE_COMPUTE_SHADERS
				DescriptorData params[7] = {};
				params[0].pName = "LPVGrid";
				params[0].ppTextures = pWorkingTex + 3 * ((i & 0x1)) + 0;
				params[0].mCount = pAura->mCascadeCount;
				params[1].pName = "workCoeffs0";
				params[1].ppTextures = pWorkingTex + 3 * !(i & 0x1) + 0;
				params[2].pName = "workCoeffs1";
				params[2].ppTextures = pWorkingTex + 3 * !(i & 0x1) + 1;
				params[3].pName = "workCoeffs2";
				params[3].ppTextures = pWorkingTex + 3 * !(i & 0x1) + 2;
				params[4].pName = "RWGrid0";
				params[4].ppTextures = &pTex[0];
				params[5].pName = "RWGrid1";
				params[5].ppTextures = &pTex[1];
				params[6].pName = "RWGrid2";
				params[6].ppTextures = &pTex[2];
				updateDescriptorSet(pRenderer, cascade * 2 + i, pAura->pDescriptorSetLightPropagateN, 7, params);
#else
				DescriptorData params[1] = {};
				params[0].pName = "LPVGrid";
				params[0].ppTextures = pWorkingTex + 3 * ((i & 0x1)) + 0;
				params[0].mCount = pAura->mCascadeCount;
				updateDescriptorSet(pRenderer, cascade * 2 + i, pAura->pDescriptorSetLightPropagateN, 1, params);
#endif
			}
		}

		*ppAura = pAura;
	}

	void loadCPUPropagationResources(Renderer* pRenderer, Aura* pAura)
	{
#ifdef ENABLE_CPU_PROPAGATION
		pAura->m_CPUContexts = (LightPropagationCPUContext**)aura::alloc(pAura->mCascadeCount * sizeof(LightPropagationCPUContext*));
		for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
		{
			pAura->m_CPUContexts[i] = (LightPropagationCPUContext*)aura::alloc(NUM_GRIDS_PER_CASCADE * sizeof(LightPropagationCPUContext));
			for (uint32_t j = 0; j < NUM_GRIDS_PER_CASCADE; ++j)
			{
				pAura->m_CPUContexts[i][j].load(pRenderer);
			}
		}
#endif
	}

	void unloadCPUPropagationResources(Renderer* pRenderer, ITaskManager* pTaskManager, Aura* pAura)
	{
#ifdef ENABLE_CPU_PROPAGATION
		for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
		{
			for (uint32_t j = 0; j < NUM_GRIDS_PER_CASCADE; ++j)
			{
				pAura->m_CPUContexts[i][j].unload(pRenderer, pTaskManager);
			}

			aura::dealloc(pAura->m_CPUContexts[i]);
		}

		aura::dealloc(pAura->m_CPUContexts);
#endif
	}

	void removeAura(Renderer* pRenderer, ITaskManager* pTaskManager, Aura* pAura)
	{
		removeDescriptorSet(pRenderer, pAura->pDescriptorSetInjectRSMLight);
		removeDescriptorSet(pRenderer, pAura->pDescriptorSetLightPropagate1);
		removeDescriptorSet(pRenderer, pAura->pDescriptorSetLightPropagateN);
		removeDescriptorSet(pRenderer, pAura->pDescriptorSetLightCopy);
		removeDescriptorSet(pRenderer, pAura->pDescriptorSetVisualizeLPV);
		/************************************************************************/
		// Remove uniform buffers
		/************************************************************************/
		for (uint32_t i = 0; i < MAX_FRAMES; ++i)
		{
			for (uint32_t j = 0; j < pAura->mCascadeCount; ++j)
			{
				removeBuffer(pRenderer, pAura->pUniformBufferInjectRSM[i][j]);				
			}

			aura::dealloc( pAura->pUniformBufferInjectRSM[i]);

			removeBuffer(pRenderer, pAura->pUniformBufferVisualizationData[i]);
		}
		/************************************************************************/
		// Remove pipelines
		/************************************************************************/
		removePipeline(pRenderer, pAura->pPipelineInjectRSMLight);
		removePipeline(pRenderer, pAura->pPipelineLightPropagate1[0]);
		removePipeline(pRenderer, pAura->pPipelineLightPropagateN[0]);
		removePipeline(pRenderer, pAura->pPipelineLightCopy);
		removePipeline(pRenderer, pAura->pPipelineVisualizeLPV);

		/************************************************************************/
		// Remove root signatures
		/************************************************************************/
		removeRootSignature(pRenderer, pAura->pRootSignatureInjectRSMLight);
		removeRootSignature(pRenderer, pAura->pRootSignatureLightPropagate1);
		removeRootSignature(pRenderer, pAura->pRootSignatureLightPropagateN);
		removeRootSignature(pRenderer, pAura->pRootSignatureLightCopy);
		removeRootSignature(pRenderer, pAura->pRootSignatureVisualizeLPV);
		/************************************************************************/
		// Unload Shaders
		/************************************************************************/
		removeShader(pRenderer, pAura->pShaderInjectRSMLight);
		removeShader(pRenderer, pAura->pShaderLightPropagate1[0]);
		removeShader(pRenderer, pAura->pShaderLightPropagateN[0]);
		removeShader(pRenderer, pAura->pShaderLightCopy);
		removeShader(pRenderer, pAura->pShaderLPVVisualize);
		/************************************************************************/
		// Remove Samplers
		/************************************************************************/
		removeSampler(pRenderer, pAura->pSamplerPointBorder);
		/************************************************************************/
		// CPU contexts
		/************************************************************************/
		unloadCPUPropagationResources(pRenderer, pTaskManager, pAura);

		/************************************************************************/
		// Remove render targets
		/************************************************************************/
		for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
		{
			removeLightPropagationCascade(pRenderer, pAura->pCascades[i]);
		}

		for (uint32_t i = 0; i < 6; ++i)
			removeLightPropagationGrid(pRenderer, pAura->pWorkingGrids[i]);

		aura::dealloc(pAura->pCascades);
		/************************************************************************/
		/************************************************************************/

		removeAsynchronousResources();

		aura::dealloc(pAura);
	}

	void getShaderMacros(Aura* pAura, ShaderMacro pMacros[4], uint32_t* pCount)
	{
		if (pAura->mOptions & LPV_NO_FRESNEL)
			pMacros[(*pCount)++] = { "NO_FRESNEL", "1" };

		if (pAura->mOptions & LPV_ALLOW_OCCLUSION)
			pMacros[(*pCount)++] = { "ALLOW_OCCLUSION", "1" };

		if (pAura->mOptions&LPV_UNPACK_NORMAL)
			pMacros[(*pCount)++] = { "UNPACK_NORMAL", "1" };

		if (pAura->mOptions&LPV_SCALAR_SPECULAR)
			pMacros[(*pCount)++] = { "SCALAR_SPECULAR", "1" };
	}

	bool doAlternateGPUUpdates(Aura* pAura)
	{
		return (pAura->mParams.bAlternateGPUUpdates
#ifdef ENABLE_CPU_PROPAGATION
			&& !pAura->mParams.bUseCPUPropagation
#endif
			);
	}

	void beginFrame(Renderer* pRenderer, Aura* pAura, const vec3& camPos, const vec3& camDir)
	{
		if (doAlternateGPUUpdates(pAura))
		{
			pAura->mGPUPropagationCurrentGrid = (pAura->mGPUPropagationCurrentGrid + 1) % pAura->mCascadeCount;

			beginFrame(pAura->pCascades[pAura->mGPUPropagationCurrentGrid], camPos, camDir);
		}
		else
		{
			for (size_t i = 0; i < pAura->mCascadeCount; ++i)
				beginFrame(pAura->pCascades[i], camPos, camDir);
		}
	}

	void endFrame(Renderer* pRenderer, Aura* pAura)
	{
		++pAura->mFrameIdx;
		if (pAura->mFrameIdx >= MAX_FRAMES)
			pAura->mFrameIdx = 0;

		mapAsynchronousResources(pRenderer);
	}

	void setCascadeCenter(Aura* pAura, uint32_t Cascade, const vec3& center)
	{
		setGridCenter(pAura->pCascades[Cascade], center);
	}

	void getGridBounds(Aura* pAura, const mat4& worldToLocal, Box* bounds)
	{
		for (size_t i = 0; i < pAura->mCascadeCount; ++i)
			bounds[i] = getGridBounds(pAura->pCascades[i], worldToLocal);
	}

	uint32_t getCascadesToUpdateMask(Aura* pAura)
	{
		if (doAlternateGPUUpdates(pAura))
		{
			return (0x0001 << pAura->mGPUPropagationCurrentGrid);
		}
		else
		{
			uint32_t res = 0;
			for (uint i = 0; i < pAura->mCascadeCount; ++i)
				res += (0x0001 << i);

			return res;
		}
	}

	void injectRSM(Cmd* pCmd, Renderer* pRenderer, Aura* pAura, uint32_t iVolume, const mat4 &invVP, const vec3 &camDir, uint32_t rtWidth, uint32_t rtHeight, float viewAreaForUnitDepth, Texture* baseRT, Texture* normalRT, Texture* depthRT)
	{
		if (doAlternateGPUUpdates(pAura) && (pAura->mGPUPropagationCurrentGrid != iVolume))
			return;

		float RSMSurfelAreaScaleFactor = viewAreaForUnitDepth / (float)(rtWidth*rtHeight);

		LightPropagationCascade* pCascade = pAura->pCascades[iVolume];

		//const int	RSMRes[4] = {rtWidth,rtHeight,0,0};
		uint32_t RSMRes[2];
		RSMRes[0] = rtWidth;
		RSMRes[1] = rtHeight;

		//	Make sure each surfel potential is scaled according to the ratio of grid area 
		//	and surfel area.

		float gridArea = pCascade->mGridSpan * pCascade->mGridSpan;
		float gridCellArea = gridArea / (GridRes*GridRes);
		float blockingPotentialFactor = RSMSurfelAreaScaleFactor / gridCellArea;
#ifdef	PRESCALE_LIGHT_VALUES
		blockingPotentialFactor *= pCascade->mGridIntensity;
#endif	//	PRESCALE_LIGHT_VALUES

#if USE_COMPUTE_SHADERS
		Texture* ppTextures[NUM_GRIDS_PER_CASCADE];
		TextureBarrier barriers[NUM_GRIDS_PER_CASCADE] = {};
		{
			for (uint32_t j = 0; j < NUM_GRIDS_PER_CASCADE; ++j)
			{
				getTextureFromRenderTarget(pCascade->pLightGrids[j], &ppTextures[j]);
				barriers[j] = { ppTextures[j], RESOURCE_STATE_LPV, RESOURCE_STATE_RENDER_TARGET };
			}
		}
		cmdResourceBarrier(pCmd, 0, NULL, NUM_GRIDS_PER_CASCADE, barriers, 0, NULL);
#endif

		LightInjectionData data = {};
		data.camDir = camDir;
		data.invMvp = transpose(invVP);
		data.RSMRes = { RSMRes[0], RSMRes[1] };
		data.scaleFactor = blockingPotentialFactor;
		data.smoothGridPosOffset = pCascade->mInjectState.mSmoothTCOffset;
		data.WorldToGridScale = pCascade->mInjectState.mWorldToGridScale;
		data.WorldToGridTranslate = pCascade->mInjectState.mWorldToGridTranslate;
		updateUniformBuffer(pRenderer, pAura->pUniformBufferInjectRSM[pAura->mFrameIdx][iVolume], 0, &data, 0, sizeof(LightInjectionData));

		LoadActionsDesc loadActions = {};
		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
		{
			loadActions.mClearColorValues[i] = { {{0, 0, 0, 0}} };
			loadActions.mLoadActionsColor[i] = LOAD_ACTION_CLEAR;
		}

		cmdBindRenderTargets(pCmd, NUM_GRIDS_PER_CASCADE, pCascade->pLightGrids, NULL, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)GridRes, (float)GridRes, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, GridRes, GridRes);
		cmdBindPipeline(pCmd, pAura->pPipelineInjectRSMLight);
		DescriptorData params[4] = {};
		params[0].pName = "uniforms";
		params[0].ppBuffers = &pAura->pUniformBufferInjectRSM[pAura->mFrameIdx][iVolume];
		params[1].pName = "tDepth";
		params[1].ppTextures = &depthRT;
		params[2].pName = "tNormal";
		params[2].ppTextures = &normalRT;
		params[3].pName = "tBase";
		params[3].ppTextures = &baseRT;
		updateDescriptorSet(pRenderer, pAura->mFrameIdx * pAura->mCascadeCount + iVolume, pAura->pDescriptorSetInjectRSMLight, 4, params);
		cmdBindDescriptorSet(pCmd, pAura->mFrameIdx * pAura->mCascadeCount + iVolume, pAura->pDescriptorSetInjectRSMLight);
		cmdDraw(pCmd, RSMRes[0] * RSMRes[1], 0);

		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

		//	Igor: It's pretty odd, but I need this scaler in order to make solid wall
		//	occluder take the whole cell. Pretty odd.
		RSMSurfelAreaScaleFactor *= 2;

		//
		// in case we use occlusion volumes ...
		// 
		//	TODO: Igor: control this a different way
		if (!(pAura->mOptions & LPV_ALLOW_OCCLUSION)
#ifdef ENABLE_CPU_PROPAGATION
			|| pAura->mParams.bUseCPUPropagation
#endif
			)
			return;

		// TODO
		//m_Cascades[iVolume].injectOccluders(
		//	invVP, camDir, rtWidth, rtHeight,
		//	RSMSurfelAreaScaleFactor,
		//	normalRT, depthRT,
		//	m_shInjectOccluder, m_bsAddOneOne, m_ssPointBorder, m_shCopyOccluder, m_WorkingGrids);
	}

	void propagateLight(Cmd* pCmd, Renderer* pRenderer, Aura* pAura, uint32_t cascade)
	{
		char name[128] = {};
		sprintf(name, "Cascade #%u", cascade);
		cmdBeginDebugMarker(pCmd, 1.0f, 0.0f, 0.0f, name);

		Pipeline* pPipelinePropagate1 = pAura->pPipelineLightPropagate1[pAura->mParams.bUseMultipleReflections];
		Pipeline* pPipelinePropagateN = pAura->pPipelineLightPropagateN[pAura->mParams.bUseMultipleReflections];
		LightPropagationCascade* pCascade = pAura->pCascades[cascade];
		Texture* pTex[NUM_GRIDS_PER_CASCADE] = {};
		Texture* pWorkingTex[NUM_GRIDS_PER_CASCADE * 2] = {};

		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
			getTextureFromRenderTarget(pCascade->pLightGrids[i], &pTex[i]);
		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE * 2; ++i)
			getTextureFromRenderTarget(pAura->pWorkingGrids[i], &pWorkingTex[i]);

#if USE_COMPUTE_SHADERS
		/************************************************************************/
		// 1st propagation step
		/************************************************************************/
		cmdBindPipeline(pCmd, pPipelinePropagate1);
		cmdBindDescriptorSet(pCmd, cascade, pAura->pDescriptorSetLightPropagate1);
		cmdBindPushConstants(pCmd, pAura->pRootSignatureLightPropagate1, "PropagationSetupRootConstant", &pAura->mParams.fPropagationScale);
		cmdDispatch(pCmd, GridRes / WorkGroupSize, GridRes / WorkGroupSize, GridRes / WorkGroupSize);
		/************************************************************************/
		// Barriers
		/************************************************************************/
		TextureBarrier barriers[NUM_GRIDS_PER_CASCADE * 2] = {};
		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
		{
			barriers[i * 2] = { pTex[i], RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS };
			barriers[i * 2 + 1] = { pWorkingTex[i], RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_SHADER_RESOURCE };
		}
		cmdResourceBarrier(pCmd, 0, NULL, NUM_GRIDS_PER_CASCADE * 2, barriers, 0, NULL);
		/************************************************************************/
		//	Add propagated light to the final grid
		/************************************************************************/
		cmdBindPipeline(pCmd, pAura->pPipelineLightCopy);
		cmdBindDescriptorSet(pCmd, cascade, pAura->pDescriptorSetLightCopy);
		cmdDispatch(pCmd, GridRes / WorkGroupSize, GridRes / WorkGroupSize, GridRes / WorkGroupSize);
		/************************************************************************/
		/************************************************************************/
#else
		/************************************************************************/
		// 1st propagation step
		/************************************************************************/
		cmdBindRenderTargets(pCmd, NUM_GRIDS_PER_CASCADE, pAura->pWorkingGrids, NULL, NULL, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)GridRes, (float)GridRes, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, GridRes, GridRes);
		cmdBindPipeline(pCmd, pPipelinePropagate1);
		cmdBindDescriptorSet(pCmd, cascade, pAura->pDescriptorSetLightPropagate1);
		cmdBindPushConstants(pCmd, pAura->pRootSignatureLightPropagate1, "PropagationSetupRootConstant", &pAura->mParams.fPropagationScale);
		cmdDrawInstanced(pCmd, 3, 0, GridRes, 0);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		/************************************************************************/
		// Barriers
		/************************************************************************/
		TextureBarrier barriers[NUM_GRIDS_PER_CASCADE * 2] = {};
		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
		{
			barriers[i * 2] = { pTex[i], RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET };
			barriers[i * 2 + 1] = { pWorkingTex[i], RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE };
		}
		cmdResourceBarrier(pCmd, 0, NULL, NUM_GRIDS_PER_CASCADE * 2, barriers, 0, NULL);
		/************************************************************************/
		//	Add propagated light to the final grid
		/************************************************************************/
		cmdBindRenderTargets(pCmd, NUM_GRIDS_PER_CASCADE, pCascade->pLightGrids, NULL, NULL, NULL, NULL, -1, -1);
		cmdBindPipeline(pCmd, pAura->pPipelineLightCopy);
		cmdBindDescriptorSet(pCmd, cascade * 2 + 0, pAura->pDescriptorSetLightCopy);
		cmdDrawInstanced(pCmd, 3, 0, GridRes, 0);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		/************************************************************************/
		/************************************************************************/
#endif
		/************************************************************************/
		// Light propagation step 2
		/************************************************************************/
		bool bPhase = true;
		const uint32_t nPropagationSteps = pAura->mParams.iPropagationSteps;

		//	Use ping-pong rt changes to propagate only previous step light
		for (uint32_t i = 1; i < nPropagationSteps; ++i)
		{
#if USE_COMPUTE_SHADERS
			cmdBindPipeline(pCmd, pPipelinePropagateN);
			cmdBindPushConstants(pCmd, pAura->pRootSignatureLightPropagateN, "PropagationSetupRootConstant", &pAura->mParams.fPropagationScale);
			cmdBindDescriptorSet(pCmd, cascade * 2 + !(i & 0x1), pAura->pDescriptorSetLightPropagateN);
			cmdDispatch(pCmd, GridRes / WorkGroupSize, GridRes / WorkGroupSize, GridRes / WorkGroupSize);

			for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
			{
				barriers[i * 2] = { pWorkingTex[3 * bPhase + i], RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_SHADER_RESOURCE };
				barriers[i * 2 + 1] = { pWorkingTex[3 * !bPhase + i], RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS };
			}
			cmdResourceBarrier(pCmd, 0, NULL, NUM_GRIDS_PER_CASCADE * 2, barriers, 0, NULL);
#else	//	USE_COMPUTE_SHADERS

#ifdef	PROPAGATE_ACCUMULATE_ONE_PASS
			// Tim: set the pWorkking grids ping pong RTs as the first 3, just as what
			// was done before, but always bind the m_LightGrids RTs as the second 3 RTs
			// to hold the accumulated data
			TextureID bufferRTs[6] = { pWorkingGrids[3 * bPhase].Data(), pWorkingGrids[1 + 3 * bPhase].Data(), pWorkingGrids[2 + 3 * bPhase].Data(), m_LightGrids[0].Data(), m_LightGrids[1].Data(), m_LightGrids[2].Data() };
			pRenderer->changeRenderTargets(bufferRTs, elementsOf(bufferRTs), TEXTURE_NONE);

#else	// PROPAGATE_ACCUMULATE_ONE_PASS
			cmdBindRenderTargets(pCmd, NUM_GRIDS_PER_CASCADE, pAura->pWorkingGrids + 3 * bPhase, NULL, NULL, NULL, NULL, -1, -1);
#endif	// PROPAGATE_ACCUMULATE_ONE_PASS

			cmdBindPipeline(pCmd, pPipelinePropagateN);
			cmdBindDescriptorSet(pCmd, cascade * 2 + !(i & 0x1), pAura->pDescriptorSetLightPropagateN);
			cmdBindPushConstants(pCmd, pAura->pRootSignatureLightPropagateN, "PropagationSetupRootConstant", &pAura->mParams.fPropagationScale);
			cmdDrawInstanced(pCmd, 3, 0, GridRes, 0);

#ifdef PROPOGATE_ACCUMULATE_ONE_PASS
#else
			cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
#endif

#endif	//	USE_COMPUTE_SHADERS

#if	!defined(PROPAGATE_ACCUMULATE_ONE_PASS) && !USE_COMPUTE_SHADERS
			for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
			{
				barriers[i * 2] = { pWorkingTex[3 * bPhase + i], RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE };
				barriers[i * 2 + 1] = { pWorkingTex[3 * !bPhase + i], RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET };
			}
			cmdResourceBarrier(pCmd, 0, NULL, NUM_GRIDS_PER_CASCADE * 2, barriers, 0, NULL);

			//	Add propagated light to the final grid
			cmdBindRenderTargets(pCmd, NUM_GRIDS_PER_CASCADE, pCascade->pLightGrids, NULL, NULL, NULL, NULL, -1, -1);
			cmdBindPipeline(pCmd, pAura->pPipelineLightCopy);
			cmdBindDescriptorSet(pCmd, cascade * 2 + (i & 0x1), pAura->pDescriptorSetLightCopy);
			cmdDrawInstanced(pCmd, 3, 0, GridRes, 0);
			cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
#endif	// PROPAGATE_ACCUMULATE_ONE_PASS

			//	flip rt and source
			bPhase = !bPhase;
		}

		for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
		{
			barriers[i] = { pWorkingTex[3 * !bPhase + i], RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_LPV };
		}
		cmdResourceBarrier(pCmd, 0, NULL, NUM_GRIDS_PER_CASCADE, barriers, 0, NULL);
		/************************************************************************/
		/************************************************************************/

		pCascade->mApplyState = pCascade->mInjectState;

		cmdEndDebugMarker(pCmd);
	}

	void propagateLight(Cmd* pCmd, Renderer* pRenderer, ITaskManager* pTaskManager, Aura* pAura)
	{
#ifdef ENABLE_CPU_PROPAGATION
		if (pAura->bUseCPUPropagationPreviousFrame != pAura->mParams.bUseCPUPropagation && pAura->mParams.bUseCPUPropagation) {
			unloadCPUPropagationResources(pRenderer, pTaskManager, pAura);
			loadCPUPropagationResources(pRenderer, pAura);
		}
		pAura->bUseCPUPropagationPreviousFrame = pAura->mParams.bUseCPUPropagation;

		if (pAura->mParams.bUseCPUPropagation)
		{
			int readIndex = pAura->mFrameIdx % pAura->mInFlightFrameCount;

			for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
			{
				pAura->m_CPUContexts[i][readIndex].readData(pCmd, pRenderer, pAura->pCascades[i]->pLightGrids, NUM_GRIDS_PER_CASCADE);
				pAura->m_CPUContexts[i][readIndex].setApplyState(pAura->pCascades[i]->mInjectState);
				pAura->m_CPUContexts[i][readIndex].eState = LightPropagationCPUContext::CAPTURED_LIGHT;
			}

			int propagateIndex = (pAura->mFrameIdx - pAura->mInFlightFrameCount) % pAura->mInFlightFrameCount;
			for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
			{
				if (LightPropagationCPUContext::CAPTURED_LIGHT == pAura->m_CPUContexts[i][propagateIndex].eState)
				{
					pAura->m_CPUContexts[i][propagateIndex].processData(pRenderer, pTaskManager, pAura->mCPUParams.eMTMode);
					pAura->m_CPUContexts[i][propagateIndex].eState = LightPropagationCPUContext::PROPAGATED_LIGHT;

					pAura->m_CPUContexts[i][propagateIndex].applyData(pCmd, pRenderer, pAura->pCascades[i]->pLightGrids);
					pAura->pCascades[i]->mApplyState = pAura->m_CPUContexts[i][propagateIndex].getApplyState();
					pAura->m_CPUContexts[i][propagateIndex].eState = LightPropagationCPUContext::APPLIED_PROPAGATION;
				}
			}
		}
		else
#endif
		{
			if (doAlternateGPUUpdates(pAura))
			{
				RenderTargetBarrier srvBarriers[NUM_GRIDS_PER_CASCADE] = {};
				for (uint32_t i = 0; i < NUM_GRIDS_PER_CASCADE; ++i)
				{
					srvBarriers[i] = { pAura->pCascades[pAura->mGPUPropagationCurrentGrid]->pLightGrids[i], RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE };
				}
				cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, NUM_GRIDS_PER_CASCADE, srvBarriers);

				propagateLight(pCmd, pRenderer, pAura, pAura->mGPUPropagationCurrentGrid);
			}
			else
			{
				RenderTargetBarrier* pSrvBarriers = (RenderTargetBarrier*)alloca(pAura->mCascadeCount * NUM_GRIDS_PER_CASCADE * sizeof(RenderTargetBarrier));
				for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
				{
					for (uint32_t j = 0; j < NUM_GRIDS_PER_CASCADE; ++j)
					{
						pSrvBarriers[i * NUM_GRIDS_PER_CASCADE + j] = { pAura->pCascades[i]->pLightGrids[j], RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE };
					}
				}
				cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, pAura->mCascadeCount * NUM_GRIDS_PER_CASCADE, pSrvBarriers);

				for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
				{
					propagateLight(pCmd, pRenderer, pAura, i);
				}
			}
		}
	}

	void getLightApplyData(Aura* pAura, const mat4& invVP, const vec3& camPos, LightApplyData* data)
	{
		data->camPos = camPos;
		data->invMvp = transpose(invVP);
		data->lumScale = float3(pAura->mParams.fFresnel, pAura->mParams.fSpecScale, pAura->mParams.fSpecPow);
		data->normalScale = float3(1.0f / GridRes);
		data->cascadeCount = pAura->mCascadeCount;
		data->GIStrength = pAura->mParams.fGIStrength;

		for (uint32_t i = 0; i < pAura->mCascadeCount; i++)
		{
			LightPropagationCascade* pCascade = pAura->pCascades[i];
			const float cellSize = (pCascade->mGridSpan / GridRes);

			data->cascade[i].WorldToGridScale = pCascade->mApplyState.mWorldToGridScale;
			data->cascade[i].WorldToGridTranslate = pCascade->mApplyState.mWorldToGridTranslate;
			data->cascade[i].cellFalloff = float4(
				1.0f / sqrf(cellSize * 0.5f),
				1.0f / sqrf(cellSize * 0.75f),
				1.0f / (cellSize),//1.0f/sqrf(cellSize*1.0f),
				1.0f / sqrf(cellSize * 1.5f));
			data->cascade[i].lightScale = pAura->mParams.fLightScale[i];
			data->cascade[i].smoothGridPosOffset = pCascade->mApplyState.mSmoothTCOffset;

				//	The 'fairest' falloffs. But not the best.
			/*data.cellFalloff = float4(
			1.0f/sqrf(cellSize*0.5f),
			1.0f/sqrf(cellSize*1.5f),
			1.0f/sqrf(cellSize*2.5f),
			1.0f/sqrf(cellSize*3.5f));*/
			/*data.cellFalloff = float4(
			1.0f/sqrf(cellSize*0.5f),
			1.0f/sqrf(cellSize*0.5f),
			1.0f/sqrf(cellSize*0.5f),
			1.0f/sqrf(cellSize*0.5f));*/
			/*data.cellFalloff = float4(
			1.0f/sqrf(cellSize*0.5f),
			1.0f/sqrf(cellSize*0.75f),
			1.0f/sqrf(cellSize*1.0f),
			1.0f/sqrf(cellSize*1.5f));
			*/
		}
	}

	void drawLpvVisualization(Cmd* cmd, Renderer* pRenderer, Aura* pAura, RenderTarget* renderTarget, RenderTarget* depthRenderTarget, const mat4& projection, const mat4& view, const mat4& inverseView, int cascadeIndex, float probeRadius)
	{
		Pipeline* pPipeline = pAura->pPipelineVisualizeLPV;

		LoadActionsDesc loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
		loadActions.mLoadActionDepth = LOAD_ACTION_LOAD;

		Texture** ppTextures = (Texture**)alloca(NUM_GRIDS_PER_CASCADE * pAura->mCascadeCount * sizeof(Texture*));
		int renderTargetBarrierCount = NUM_GRIDS_PER_CASCADE * pAura->mCascadeCount;
		RenderTargetBarrier* pRenderTargetBarriers = (RenderTargetBarrier*)alloca(renderTargetBarrierCount * sizeof(RenderTargetBarrier));
		for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
		{
			for (uint32_t j = 0; j < NUM_GRIDS_PER_CASCADE; ++j)
			{
				getTextureFromRenderTarget(pAura->pCascades[i]->pLightGrids[j], &ppTextures[i * NUM_GRIDS_PER_CASCADE + j]);
				pRenderTargetBarriers[i * NUM_GRIDS_PER_CASCADE + j ] = { pAura->pCascades[i]->pLightGrids[j], RESOURCE_STATE_LPV, RESOURCE_STATE_SHADER_RESOURCE };
			}
		}

		Texture* depthTexture = (Texture*)alloca(sizeof(Texture*));
		getTextureFromRenderTarget(depthRenderTarget, &depthTexture);
		TextureBarrier depthTextureBarrier[] = {
			{ depthTexture, RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_DEPTH_WRITE }
		};
		cmdResourceBarrier(cmd, 0, NULL, 1, depthTextureBarrier, renderTargetBarrierCount, pRenderTargetBarriers);

		cmdBindRenderTargets(cmd, 1, &renderTarget, depthRenderTarget, &loadActions, NULL, NULL, -1, -1);
		cmdBindPipeline(cmd, pPipeline);

		aura::LightPropagationCascade* cascade = pAura->pCascades[cascadeIndex];

		VisualizationData data = {};
		data.GridToCamera = transpose(view * cascade->mApplyState.mGridToWorld);
		data.projection = transpose(projection);
		data.invView = transpose(inverseView);
		data.probeRadius = probeRadius;
		data.lightScale = pAura->mParams.fLightScale[cascadeIndex];

		updateUniformBuffer(pRenderer, pAura->pUniformBufferVisualizationData[pAura->mFrameIdx], 0, &data, 0, sizeof(data));

		DescriptorData params[2] = {};
		params[0].pName = "uniforms";
		params[0].ppBuffers = &pAura->pUniformBufferVisualizationData[pAura->mFrameIdx];
		params[1].pName = "LPVGrid";
		params[1].ppTextures = &ppTextures[cascadeIndex * pAura->mCascadeCount + 0];
		params[1].mCount = pAura->mCascadeCount;
		updateDescriptorSet(pRenderer, pAura->mFrameIdx, pAura->pDescriptorSetVisualizeLPV, 2, params);
		cmdBindDescriptorSet(cmd, pAura->mFrameIdx, pAura->pDescriptorSetVisualizeLPV);
		
		cmdDraw(cmd, QuadVertexCount * GridRes * GridRes * GridRes, 0);

		cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

		// Reset barrier states.
		for (uint32_t i = 0; i < pAura->mCascadeCount; ++i)
		{
			for (uint32_t j = 0; j < NUM_GRIDS_PER_CASCADE; ++j)
			{
				pRenderTargetBarriers[i * NUM_GRIDS_PER_CASCADE + j].mNewState = RESOURCE_STATE_LPV;
				pRenderTargetBarriers[i * NUM_GRIDS_PER_CASCADE + j].mCurrentState = RESOURCE_STATE_SHADER_RESOURCE;
			}
		}

		depthTextureBarrier[0].mCurrentState = RESOURCE_STATE_DEPTH_WRITE;
		depthTextureBarrier[0].mNewState = RESOURCE_STATE_SHADER_RESOURCE;
		cmdResourceBarrier(cmd, 0, NULL, 1, depthTextureBarrier, renderTargetBarrierCount, pRenderTargetBarriers);
	}

	//	Igor: this was used to calculate solid angles for the box faces.
	//	This code is left for the reference
	/*
	float tri_projection(float3 a, float3 b, float3 c)
	{
	float det = abs(dot(a,cross(b,c)));

	float al = length(a);
	float bl = length(b);
	float cl = length(c);

	float div = al*bl*cl + dot(a,b)*cl + dot(a,c)*bl + dot(b,c)*al;
	float at = atan2(det, div);
	if(at < 0) at += PI; // If det>0 && div<0 atan2 returns < 0, so add pi.
	float omega = 2.0f * at;

	return omega;
	}

	void calcSolidAngles()
	{
	//	This is used to calculate the solid angle of cude faces for the propagation.
	//	Note that the sum of all faces should be 4*pi

	float3 a1 = float3(-0.5f, 0.5f, 0.5f);
	float3 a2 = float3(0.5f, 0.5f, 0.5f);

	float3 b1 = float3(-0.5f, 0.5f, 1.5f);
	float3 b2 = float3(0.5f, 0.5f, 1.5f);

	float3 c1 = float3(-0.5f, -0.5f, 1.5f);
	float3 c2 = float3(0.5f, -0.5f, 1.5f);

	//	4 faces per neighbour
	float faceAAngle = tri_projection(a1,a2,b1) + tri_projection(a2,b1,b2);
	//	0.42343134 sr

	//	1 face per neighbour
	float faceBAngle = tri_projection(b1,b2,c1) + tri_projection(b2,c1,c2);
	//	0.40066966 sr

	float AllFacesAngle = (faceAAngle*4 + faceBAngle)*6;
	//	12.566370 sr

	float FullAngle = 4*PI;
	//	12.566371 sr

	//	Error is pretty small
	int i=0;

	}
	*/

	// Ref : LPV load
	//////////////////////////////
	//	Create last to speedup shader compilation to get compile errors earlier
	//m_Cascades.push_back(LightPropagationCascade(12.0f));
	//m_Cascades.push_back(LightPropagationCascade(10*120.0f));
	//m_Cascades.push_back(LightPropagationCascade(20*120.0f));
	//m_Cascades.push_back(LightPropagationCascade(6*120.0f));

	//m_Cascades.push_back(LightPropagationCascade(30.0f, 0.078125f));
	//	Sponza demo
	//m_Cascades.push_back(LightPropagationCascade(50.0f, 0.3125f));	//	Orig
	//m_Cascades.push_back(LightPropagationCascade(110.0f, 1.25f));		//	Orig
	//m_Cascades.push_back(LightPropagationCascade(220.0f, 5.0f));		//	Orig
	//m_Cascades.push_back(LightPropagationCascade(440.0f, 10.0f));		//	Orig

	//	RawK settings. Only 2 cascades are used. 3d cascade has issues with precision. Need to fix this situation.
	//m_Cascades.push_back(LightPropagationCascade(440.0f, 20.0f));
	//m_Cascades.push_back(LightPropagationCascade(880.0f, 80.0f));
	////m_Cascades.push_back(LightPropagationCascade(1760.0f, 320.0f));

	//m_Cascades.push_back(LightPropagationCascade(3520.0f, 1280.0f));
}