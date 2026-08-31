#pragma once

#include "DObject/AssetPath.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class DObject;
}

namespace Durin::Editor
{
	struct FRetainedAssetState;

	// Keeps one canonically identified editor asset alive while any acquisition handle exists.
	class FRetainedAsset
	{
	public:
		FRetainedAsset() = default;

		DURINED_API auto Get() const -> DObject*;
		DURINED_API auto GetPath() const -> const FObjectPath*;
		explicit operator bool() const { return Get() != nullptr; }

	private:
		friend class FAssetRetentionService;
		explicit FRetainedAsset(std::shared_ptr<FRetainedAssetState> InState)
			: State(std::move(InState))
		{
		}

		std::shared_ptr<FRetainedAssetState> State;
	};

	// Coalesces editor asset lifetime by canonical virtual identity on the game thread.
	class FAssetRetentionService
	{
	public:
		DURINED_API static auto Acquire(
			const FObjectPath& Path,
			FRetainedAsset& OutAsset,
			std::string& OutError) -> bool;

		DURINED_API static auto NumRetained() -> size_t;
	};
} // namespace Durin::Editor
