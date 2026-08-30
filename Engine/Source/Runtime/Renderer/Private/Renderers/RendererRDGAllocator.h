#pragma once

#include "RendererAPI.h"
#include "RDG.h"

#include <memory>
#include <span>
#include <vector>

namespace Durin
{
	class FRendererResourceCoordinator;

	// Attributes retained RDG bytes without participating in allocation identity.
	enum class ERDGAllocationObservation : uint8
	{
		Scene,
		GBuffer,
		GroundTruthAmbientOcclusion,
		ContactFragment,
		ContactCompute,
		VolumetricCloudShadowFragment,
		VolumetricCloudShadowCompute,
		DeferredDirectional,
		GBufferDebug,
		VolumetricCloudFragment,
		VolumetricCloudCompute,
		VolumetricCloudComposite,
		Count,
	};

	// Defines the structural retained-memory ceiling for descriptor-driven RDG
	// allocation without changing feature attribution or eviction priority.
	struct FRendererRDGAllocationPolicy final
	{
		static constexpr uint64 MaximumRetainedBytes =
			640ull * 1024ull * 1024ull;

		static constexpr auto IsBatchWithinStructuralBudget(uint64 Bytes) -> bool
		{
			return Bytes <= MaximumRetainedBytes;
		}
	};

	class RENDERER_API FRendererRDGAllocator final : public FRDGAllocator
	{
	public:
		explicit FRendererRDGAllocator(
			FRendererResourceCoordinator& InCoordinator);
		~FRendererRDGAllocator();

		auto GetObservedRetainedBytes_RenderThread(
			ERDGAllocationObservation Observation) const -> uint64;
		auto Release_RenderThread() -> void;
		auto Allocate(std::span<const FRDGAllocationRequest> Requests,
			FRDGAllocatedResources& OutResources, std::string& OutError)
			-> bool override;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
