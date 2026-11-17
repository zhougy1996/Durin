#pragma once

#include "CoreMinimal.h"
#include "PipelineStateCache.h"

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
	struct FPreparedPostProcessMaterial
	{
		std::shared_ptr<const FMaterialProgram> Program;
		FMaterialRenderBinding Material;
		FShaderRHIRef Fragment;
		FShaderRHIRef Vertex;
		FGraphicsPipelineStateRef Pipeline;
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

		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList, bool bPresentOutput, bool bEnableFXAA,
			bool bHasEditorAssistance, EPixelFormat OutputFormat) -> bool;
		static auto DescribeSceneTargets(uint32 Width, uint32 Height)
			-> std::array<FRHITextureCreateDesc, 2>;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* SceneColor,
			uint32 Width,
			uint32 Height,
			bool bPresentOutput,
			bool bEnableFXAA,
			bool bHasEditorAssistance,
			EPixelFormat OutputFormat,
			float ExposureEV) -> void;
		auto PrepareMaterial_RenderThread(FRHICommandListImmediate& Commands,
			const FSceneView& View, const FSceneViewRenderOptions& Options, FScene* Scene, uint32 Width, uint32 Height,
			bool bRejectPreparation = false, bool bRejectBindings = false)
			-> std::shared_ptr<const FPreparedPostProcessMaterial>;
		auto DrawMaterial_RenderThread(FRHICommandListImmediate& Commands,
			const FPreparedPostProcessMaterial& Prepared, FRHITexture* Input0,
			FRHITexture* Depth, const FMaterialSceneTextureUniform& Uniform) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
