#pragma once

#include "RendererAPI.h"
#include "RenderGraph.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Durin
{
	class FRendererResourceCoordinator;

	enum class ERendererTransientTargetGroup : uint8
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

	class RENDERER_API FRendererTransientTargetPool final : public FRDGAllocator
	{
	public:
		struct FLease
		{
			std::vector<FTextureRHIRef> Textures;
			uint64 ActiveBytes = 0;

			auto Get(size_t Index) const -> FRHITexture*
			{
				return Index < Textures.size() ? Textures[Index].GetReference()
					: nullptr;
			}
		};

		explicit FRendererTransientTargetPool(
			FRendererResourceCoordinator& InCoordinator);
		~FRendererTransientTargetPool();

		auto AcquireBundle_RenderThread(
			ERendererTransientTargetGroup Group,
			std::span<const FRHITextureCreateDesc> Descriptions,
			uint64 MaximumRetainedBytes) -> std::optional<FLease>;
		auto GetRetainedBytes_RenderThread(
			ERendererTransientTargetGroup Group) const -> uint64;
		auto GetTotalRetainedBytes_RenderThread() const -> uint64;
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
