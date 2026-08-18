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
		// Identifies the complete contact-visibility producer selected for one view.
		enum class ERoute : uint8
		{
			FactorOne,
			Compute,
			Fragment
		};

		// Records the bounded decision reason without exposing backend identity.
		enum class ERouteReason : uint8
		{
			DisabledOrUnneeded,
			InvalidInputs,
			InvalidExtent,
			Compute,
			ComputePayloadUnavailable,
			ComputeTargetUnavailable,
			ComputeExtentUnsupported,
			FragmentUnavailable,
			Count
		};
		using FTimingQuerySink = void (*)(
			const FGPUTimingQueryRHIRef& Query, ERoute Route);
		static auto SetTimingQuerySink(FTimingQuerySink Sink) -> void;

		// Carries only backend-neutral facts consumed by the pure route decision.
		struct FRouteInputs
		{
			bool bRequested = false;
			bool bInputsValid = false;
			bool bComputePayloadReady = false;
			bool bComputeTargetReady = false;
			bool bFragmentReady = false;
			uint32 Width = 0;
			uint32 Height = 0;
			uint32 MaxGroupCountX = 0;
			uint32 MaxGroupCountY = 0;
		};

		// Publishes one route and its diagnostic reason atomically.
		struct FRouteDecision
		{
			ERoute Route = ERoute::FactorOne;
			ERouteReason Reason = ERouteReason::DisabledOrUnneeded;
		};

		// Returns either a complete sampled visibility result or factor one.
		struct FRenderResult
		{
			FRHITexture* Visibility = nullptr;
			ERoute Route = ERoute::FactorOne;
			ERouteReason Reason = ERouteReason::DisabledOrUnneeded;
		};

		static constexpr uint64 BytesPerPixel = 1;
		static constexpr uint64 MaximumRetainedBytesPerRoute =
			16ull * 1024ull * 1024ull;
		static constexpr uint64 MaximumRetainedBytes =
			2ull * MaximumRetainedBytesPerRoute;
		static constexpr uint32 ThreadGroupSize = 8;
		static constexpr auto CalculateTargetBytes(uint32 Width, uint32 Height)
			-> uint64 { return static_cast<uint64>(Width) * Height; }
		static constexpr auto CalculateGroupCount(uint32 Extent) -> uint32
		{
			return Extent == 0 ? 0 : (Extent + ThreadGroupSize - 1) / ThreadGroupSize;
		}
		static auto SelectRoute(const FRouteInputs& Inputs) -> FRouteDecision;

		// Owns the render-targetable fragment fallback for one extent.
		struct FTargets { FTextureRHIRef Visibility; };
		// Owns the sampled/storage compute target and its validated canonical views.
		struct FComputeTargets
		{
			FTextureRHIRef Visibility;
			FTextureViewRHIRef SampledView;
			FTextureViewRHIRef StorageView;
		};

		FContactShadowVisibilityRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FContactShadowVisibilityRenderer();
		FContactShadowVisibilityRenderer(
			const FContactShadowVisibilityRenderer&) = delete;
		auto operator=(const FContactShadowVisibilityRenderer&)
			-> FContactShadowVisibilityRenderer& = delete;

		auto EnsureTargets_RenderThread(uint32 Width, uint32 Height) -> FTargets*;
		auto EnsureComputeTargets_RenderThread(uint32 Width, uint32 Height)
			-> FComputeTargets*;
		auto Render_RenderThread(
			FRHICommandListImmediate& CommandList, bool bRequested,
			FTargets* FragmentTargets, FComputeTargets* ComputeTargets,
			FRHITexture* Material, FRHITexture* Normals, FRHITexture* Surface,
			FRHITexture* Emissive, FRHITexture* SceneDepth,
			const FSceneView& View, const FVector3& LightDirection,
			uint32 Width, uint32 Height) -> FRenderResult;
		auto GetRetainedTargetBytes_RenderThread() const -> uint64;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
