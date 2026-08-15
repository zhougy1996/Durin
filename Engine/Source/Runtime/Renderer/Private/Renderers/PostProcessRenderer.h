#pragma once

#include "RHIResources.h"

#include <limits>
#include <memory>

namespace Durin
{
	class FFullscreenGeometryResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;

	// Owns post-process shaders, output pipelines, and size-keyed scene targets.
	class FPostProcessRenderer final
	{
	public:
		static constexpr uint64 SceneTargetBytesPerPixel = 24;
		static constexpr uint64 MaximumRetainedSceneTargetBytes =
			192ull * 1024ull * 1024ull;
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
			// Stores only the selected directional light's direct contribution so
			// contact shadows cannot attenuate unrelated lighting.
			FTextureRHIRef DirectionalDirect;
			// Holds contact-shadow-corrected Scene Color; read by the post
			// process when contact shadows are enabled.
			FTextureRHIRef ContactColor;
		};

		FPostProcessRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FPostProcessRenderer();

		FPostProcessRenderer(const FPostProcessRenderer&) = delete;
		auto operator=(const FPostProcessRenderer&)
			-> FPostProcessRenderer& = delete;

		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList) -> bool;
		auto EnsureSceneTargets_RenderThread(uint32 Width, uint32 Height)
			-> FSceneTargets*;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* SceneColor,
			uint32 Width,
			uint32 Height,
			bool bPresentOutput,
			bool bEnableFXAA,
			bool bHasEditorAssistance,
			float ExposureEV) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
