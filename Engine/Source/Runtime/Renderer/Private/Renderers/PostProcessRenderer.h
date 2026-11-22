#pragma once

#include "CoreMinimal.h"
#include "PipelineStateCache.h"

#include "Shader/GlobalShader.h"
#include "RHIResources.h"
#include "RHIShaderParameters.h"
#include "Materials/MaterialRenderTypes.h"

namespace Durin
{
	class FFullscreenGeometryResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class FScene;
	struct FSceneView;
	struct FSceneViewRenderOptions;
	struct FMaterialSceneTextureUniform;
	class FPostProcessVertexShader;
	class FCopySceneColorFragmentShader;
	class FFXAAFragmentShader;
	class FBloomFragmentShader;
	enum class EBloomStage : uint32 { Extract, Downsample, Upsample };
	struct FPreparedBloom
	{
		TShaderMapRef<FBloomFragmentShader> FragmentShader;
		FGraphicsPipelineStateInitializer PipelineInitializer;
		FSamplerRHIRef Sampler;
	};

	class FWaterSnapshotFragmentShader;
	class FWaterReflectionFragmentShader;
	struct FPreparedWaterSnapshot
	{
		TShaderMapRef<FPostProcessVertexShader> VertexShader;
		TShaderMapRef<FWaterSnapshotFragmentShader> FragmentShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateInitializer Pipeline;
	};

	struct FPreparedWaterReflection
	{
		TShaderMapRef<FPostProcessVertexShader> VertexShader;
		TShaderMapRef<FWaterReflectionFragmentShader> FragmentShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateInitializer Pipeline;
	};
	struct FWaterReflectionInputs
	{
		FRHITexture* Coverage = nullptr;
		FRHITexture* Depth = nullptr;
		FRHITexture* NormalRoughness = nullptr;
		FRHITexture* SpecularF0 = nullptr;
		FRHITexture* Environment = nullptr;
		FRHITexture* BrdfLut = nullptr;
		FRHISampler* Sampler = nullptr;
		FRHIUniformBufferRange Lighting;
	};

	struct FPreparedPostProcess
	{
		TShaderMapRef<FPostProcessVertexShader> VertexShader;
		TShaderMapRef<FCopySceneColorFragmentShader> CopyFragmentShader;
		TShaderMapRef<FFXAAFragmentShader> FXAAFragmentShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateInitializer PipelineInitializer;
		FSamplerRHIRef SceneColorSampler;
		std::shared_ptr<const FPreparedBloom> Bloom;
	};

	struct FPreparedPostProcessMaterial
	{
		std::shared_ptr<const FMaterialProgram> Program;
		FMaterialRenderBinding Material;
		FShaderRHIRef Fragment;
		FShaderRHIRef Vertex;
		FVertexDeclarationRHIRef PipelineVertexDeclaration;
		FGraphicsPipelineStateInitializer PipelineInitializer;
		FTextureRHIRef Output;
		std::vector<FRHIShaderParameterResource> Bindings;
		std::vector<TRefCountPtr<FRHIResource>> Resources;
		bool bRejectBindings = false;
	};

	// Owns post-process shaders and output pipelines while recording into
	// caller-provided scene targets.
	class FPostProcessRenderer final
	{
	public:
		static constexpr uint64 SceneTargetBytesPerPixel = 12;
		static constexpr uint64 MaximumRetainedSceneTargetBytes =
			96ull * 1024ull * 1024ull;
		static constexpr auto CalculateSceneTargetBytes(
			uint32 Width,
			uint32 Height) -> uint64
		{
			const uint64 Pixels = static_cast<uint64>(Width) * Height;
			return Pixels > std::numeric_limits<uint64>::max()
				/ SceneTargetBytesPerPixel
				? std::numeric_limits<uint64>::max()
				: Pixels * SceneTargetBytesPerPixel;
		}

		struct FSceneTargets
		{
			FTextureRHIRef Color;
			FTextureRHIRef Depth;
		};

		FPostProcessRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FPostProcessRenderer();

		FPostProcessRenderer(const FPostProcessRenderer&) = delete;
		auto operator=(const FPostProcessRenderer&)
			-> FPostProcessRenderer& = delete;

		auto Prepare_RenderThread(
			FRHICommandListImmediate& CommandList, bool bEnableFXAA, const FRHIRenderTargetLayout& Targets, FPreparedPostProcess& Draw)
			-> bool;
		auto PrepareBloom_RenderThread(const FSceneView& View, const FSceneViewRenderOptions& Options,
			FPreparedPostProcess& Draw) -> void;
		auto DrawBloom_RenderThread(FRHICommandListImmediate& Commands, const FPreparedBloom& Draw,
			EBloomStage Stage, FRHITexture* Source, FRHITexture* Detail, FRHITexture* Output,
			const FSceneView& View) -> void;
		static auto DescribeSceneTargets(uint32 Width, uint32 Height)
			-> std::array<FRHITextureCreateDesc, 2>;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* SceneColor, FRHITexture* Output,
			const FPreparedPostProcess& Draw, const FClearValueBinding& ClearColor,
			bool bPresentOutput, const FSceneView& View, FRHITexture* Bloom) -> void;
		auto PrepareMaterial_RenderThread(FRHICommandListImmediate& Commands,
			const FSceneView& View, const FSceneViewRenderOptions& Options, FScene* Scene, uint32 Width, uint32 Height,
			bool bRejectPreparation = false, bool bRejectBindings = false)
			-> std::shared_ptr<const FPreparedPostProcessMaterial>;
		auto DrawMaterial_RenderThread(FRHICommandListImmediate& Commands,
			const FPreparedPostProcessMaterial& Prepared, FRHITexture* Input0,
			FRHITexture* Depth, const FMaterialSceneTextureUniform& Uniform) -> bool;
		auto PrepareWaterSnapshot_RenderThread(FRHICommandListImmediate& Commands,
			FPreparedWaterSnapshot& Out) -> bool;
		auto DrawWaterSnapshot_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
			const FPreparedWaterSnapshot& Draw, FRHITexture* Color, FRHITexture* Depth) -> void;
		auto PrepareWaterReflection_RenderThread(FPreparedWaterReflection& Out) -> bool;
		auto DrawWaterReflection_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
			const FPreparedWaterReflection& Draw, const FWaterReflectionInputs& Inputs) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
