#pragma once

#include "CoreMinimal.h"
#include "DObject/PrimaryAssetId.h"
#include "DObject/AssetPath.h"

namespace Durin
{
	struct FAssetBundleEntry
	{
		FName BundleName;
		std::vector<FTopLevelAssetPath> BundleAssets;
		auto operator==(const FAssetBundleEntry&) const -> bool = default;
	};
	struct FAssetBundleData
	{
		std::vector<FAssetBundleEntry> Bundles;
		auto AddBundleAsset(FName Name, const FTopLevelAssetPath& Asset) -> bool
		{
			if (Name.IsNone() || !Asset.IsValid()) return false;
			auto It = std::ranges::find(Bundles, Name, &FAssetBundleEntry::BundleName);
			if (It == Bundles.end()) { Bundles.push_back({Name, {}}); It = std::prev(Bundles.end()); }
			if (!std::ranges::contains(It->BundleAssets, Asset)) It->BundleAssets.push_back(Asset);
			std::ranges::sort(It->BundleAssets);
			std::ranges::sort(Bundles, {}, [](const auto& Entry) { return Entry.BundleName.ToString(); });
			return true;
		}
		auto IsValid() const -> bool
		{
			std::string Previous;
			for (const auto& Entry : Bundles)
			{
				const auto Name = Entry.BundleName.ToString();
				if (Entry.BundleName.IsNone() || (!Previous.empty() && Previous >= Name)) return false;
				Previous = Name;
				FTopLevelAssetPath PreviousPath;
				for (const auto& Path : Entry.BundleAssets)
				{
					if (!Path.IsValid() || (PreviousPath.IsValid() && !(PreviousPath < Path))) return false;
					PreviousPath = Path;
				}
			}
			return true;
		}
		auto operator==(const FAssetBundleData&) const -> bool = default;
	};

}
