#pragma once

#include "Math/MathFwd.h"
#include "RendererAPI.h"
#include "RHIResources.h"

#include <memory>

namespace Durin
{
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class FFullscreenGeometryResources;
	struct FSceneView;

	class RENDERER_API FContactShadowVisibilityRenderer final
	{
	public:
		static constexpr uint64 BytesPerPixel = 1;
		static constexpr uint64 MaximumRetainedBytes = 16ull * 1024ull * 1024ull;
		static constexpr auto CalculateTargetBytes(uint32 Width, uint32 Height)
			-> uint64 { return static_cast<uint64>(Width) * Height; }

		struct FTargets { FTextureRHIRef Visibility; };

		FContactShadowVisibilityRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FContactShadowVisibilityRenderer();
		FContactShadowVisibilityRenderer(
			const FContactShadowVisibilityRenderer&) = delete;
		auto operator=(const FContactShadowVisibilityRenderer&)
			-> FContactShadowVisibilityRenderer& = delete;

		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height) -> FTargets*;
		auto Render_RenderThread(
			FRHICommandListImmediate& CommandList, FTargets& Targets,
			FRHITexture* Material, FRHITexture* Normals, FRHITexture* Surface,
			FRHITexture* Emissive, FRHITexture* SceneDepth,
			const FSceneView& View, const FVector3& LightDirection,
			uint32 Width, uint32 Height) -> bool;
		auto GetRetainedTargetBytes_RenderThread() const -> uint64;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
