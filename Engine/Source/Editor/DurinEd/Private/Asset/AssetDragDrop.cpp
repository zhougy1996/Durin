#include "Asset/AssetDragDrop.h"

namespace Durin
{
	auto GetDraggedAssetData(std::string_view Path, std::string_view ClassName) -> std::optional<FAssetData>
	{
		FTopLevelAssetPath AssetPath;
		if (!FTopLevelAssetPath::TryCreate(Path, AssetPath)) return {};
		auto Entry = FindAssetExact(AssetPath);
		if (Entry.Asset) return std::move(Entry.Asset);
		return FAssetData{.AssetPath = AssetPath, .AssetClassName = std::string(ClassName)};
	}
}
