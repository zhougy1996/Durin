#pragma once

#include "RenderCoreAPI.h"

namespace Durin
{
	class FRendererModule;

	// Opaque, process-unique identity for one optional persistent view stream.
	class FSceneViewStateId
	{
	public:
		constexpr FSceneViewStateId() = default;

		constexpr auto IsValid() const -> bool { return Value != 0; }
		explicit constexpr operator bool() const { return IsValid(); }
		auto operator<=>(const FSceneViewStateId&) const = default;

	private:
		explicit constexpr FSceneViewStateId(uint64 InValue) : Value(InValue) {}

		uint64 Value = 0;

		friend class FRendererModule;
		friend struct FSceneViewStateIdAccess;
	};

	// Releases a renderer-owned view state without exposing its concrete storage.
	class FSceneViewStateOwner
	{
	public:
		using FReleaseViewState = void (*)(FSceneViewStateId Id);

		constexpr FSceneViewStateOwner() = default;
		FSceneViewStateOwner(const FSceneViewStateOwner&) = delete;
		auto operator=(const FSceneViewStateOwner&)
			-> FSceneViewStateOwner& = delete;

		RENDERCORE_API FSceneViewStateOwner(FSceneViewStateOwner&& Other) noexcept;
		RENDERCORE_API auto operator=(FSceneViewStateOwner&& Other) noexcept
			-> FSceneViewStateOwner&;
		RENDERCORE_API ~FSceneViewStateOwner();

		auto GetId() const -> FSceneViewStateId { return Id; }
		explicit operator bool() const { return Id.IsValid(); }
		RENDERCORE_API auto Reset() -> void;

	private:
		FSceneViewStateOwner(
			FSceneViewStateId InId,
			FReleaseViewState InReleaseViewState)
			: Id(InId), ReleaseViewState(InReleaseViewState)
		{
		}

		FSceneViewStateId Id;
		FReleaseViewState ReleaseViewState = nullptr;

		friend class FRendererModule;
		friend struct FSceneViewStateOwnerTestAccess;
	};
}
