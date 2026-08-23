#pragma once

#include "RendererAPI.h"
#include "RHIResources.h"

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Durin
{
	class FRendererResourceCoordinator;

	class RENDERER_API FRendererTransientTargetPool final
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
			std::string_view Group,
			std::span<const FRHITextureCreateDesc> Descriptions,
			uint64 MaximumRetainedBytes) -> std::optional<FLease>;
		auto GetRetainedBytes_RenderThread(std::string_view Group) const -> uint64;
		auto GetTotalRetainedBytes_RenderThread() const -> uint64;
		auto Release_RenderThread() -> void;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
