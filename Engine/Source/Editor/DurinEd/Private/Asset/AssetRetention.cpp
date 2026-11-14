#include "Asset/AssetRetention.h"
#include "Misc/UnifiedError.h"

#include "Asset/Asset.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	struct FRetainedAssetState
	{
		FSoftObjectPath Path;
		DObject* Asset = nullptr;
		TStrongObjectPtr<DObject> Root;

		FRetainedAssetState(FSoftObjectPath InPath, DObject* InAsset)
			: Path(std::move(InPath))
			, Asset(InAsset)
			, Root(InAsset)
		{
		}
	};

	namespace
	{
		auto GetRetainedAssets() -> std::unordered_map<std::string, std::weak_ptr<FRetainedAssetState>>&
		{
			static std::unordered_map<std::string, std::weak_ptr<FRetainedAssetState>> Assets;
			return Assets;
		}

		auto RemoveExpiredRetainedAssets() -> void
		{
			std::erase_if(GetRetainedAssets(), [](const auto& Entry) { return Entry.second.expired(); });
		}
	}

	auto FRetainedAsset::Get() const -> DObject*
	{
		return State ? State->Asset : nullptr;
	}

	auto FRetainedAsset::GetPath() const -> const FSoftObjectPath*
	{
		return State ? &State->Path : nullptr;
	}

	auto FAssetRetentionService::Acquire(
		const FSoftObjectPath& Path,
		FRetainedAsset& OutAsset,
		std::string& OutError) -> bool
	{
		std::optional<Durin::UnifiedError::FError> ResultError;
		checkf(IsInGameThread(), "Editor asset acquisition must run on the game thread.");
		OutAsset = {};
		OutError.clear();
		RemoveExpiredRetainedAssets();

		const std::string Key = Path.ToString();
		auto& Assets = GetRetainedAssets();
		if (const auto It = Assets.find(Key); It != Assets.end())
		{
			if (std::shared_ptr Existing = It->second.lock())
			{
				OutAsset = FRetainedAsset(std::move(Existing));
				return true;
			}
		}

		DObject* Asset = nullptr;
		const auto Result = LoadObject<DObject>(Path, nullptr, &ResultError);
		Asset = Result;
		if (!Result || Asset == nullptr)
		{
			OutError = ResultError ? ResultError->ToString()
				: std::format("Asset {} did not produce an object.", Key);
			return false;
		}

		auto State = std::make_shared<FRetainedAssetState>(Path, Asset);
		Assets.insert_or_assign(Key, State);
		OutAsset = FRetainedAsset(std::move(State));
		return true;
	}

	auto FAssetRetentionService::NumRetained() -> size_t
	{
		checkf(IsInGameThread(), "Editor asset retention diagnostics must run on the game thread.");
		RemoveExpiredRetainedAssets();
		return GetRetainedAssets().size();
	}
} // namespace Durin
