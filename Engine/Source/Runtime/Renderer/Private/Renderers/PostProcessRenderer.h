#pragma once

#include "RHIResources.h"

#include <limits>
#include <memory>
#include <optional>

namespace Durin
{
	class FFullscreenGeometryResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;

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
			FRHICommandListImmediate& CommandList) -> bool;
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
			float ExposureEV) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
