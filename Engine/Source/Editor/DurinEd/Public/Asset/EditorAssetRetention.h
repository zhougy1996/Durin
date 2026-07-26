#pragma once

#include "DObject/AssetPath.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class DObject;
	struct FRetainedEditorAssetState;

	// Keeps one canonically identified editor asset alive while any acquisition handle exists.
	class FRetainedEditorAsset
	{
	public:
		FRetainedEditorAsset() = default;

		DURINED_API auto Get() const -> DObject*;
		DURINED_API auto GetPath() const -> const FAssetPath*;
		explicit operator bool() const { return Get() != nullptr; }

	private:
		friend class FEditorAssetRetentionService;
		explicit FRetainedEditorAsset(std::shared_ptr<FRetainedEditorAssetState> InState)
			: State(std::move(InState))
		{
		}

		std::shared_ptr<FRetainedEditorAssetState> State;
	};

	// Coalesces editor asset lifetime by canonical virtual identity on the game thread.
	class FEditorAssetRetentionService
	{
	public:
		DURINED_API static auto Acquire(
			const FAssetPath& Path,
			FRetainedEditorAsset& OutAsset,
			std::string& OutError) -> bool;

		DURINED_API static auto NumRetained() -> size_t;
	};
}
