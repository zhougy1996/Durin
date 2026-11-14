#include "Thumbnail/AssetThumbnailKey.h"
#include "Serialization/BinaryFormat.h"
#include "Hash/XxHash.h"
#include "DObject/Class.h"

namespace Durin
{
	auto CaptureThumbnailKeyInput(const FAssetData& Asset,
		const DClass* RendererClass)
		-> std::expected<FAssetThumbnailKeyInput, std::string>
	{
		if (!RendererClass) return std::unexpected("The thumbnail renderer class is unavailable.");
		auto Closure = CaptureAssetDependencyClosure(Asset.AssetPath.GetPackagePath());
		if (!Closure) return std::unexpected(FormatAssetRegistryError(Closure.Result));
		const auto Root = std::ranges::find(Closure.Assets, Asset.AssetPath.GetPackagePath(), &FPackageData::PackagePath);
		if (Root == Closure.Assets.end()) return std::unexpected("The thumbnail package is unavailable.");
		const auto Current = std::ranges::find(Root->TopLevelAssets, Asset.AssetPath, &FAssetData::AssetPath);
		if (Current == Root->TopLevelAssets.end() || Current->AssetClassName != Asset.AssetClassName)
			return std::unexpected("The thumbnail asset no longer matches its registry data.");
		return FAssetThumbnailKeyInput{
			.Asset = *Current, .RendererClassName = RendererClass->GetQualifiedName().ToString(),
			.Packages = std::move(Closure.Assets)};
	}

	auto BuildAssetThumbnailCacheKey(const FAssetThumbnailKeyInput& Input) -> std::string
	{
		FBinaryWriter Writer;
		Writer.WriteString("DurinAssetThumbnailKey");
		// Bump for changes to preview output or persistent key encoding.
		constexpr uint32 ThumbnailCacheVersion = 4;
		Writer.WriteU32(ThumbnailCacheVersion);
		Writer.WriteString(Input.Asset.AssetPath.ToString());
		Writer.WriteString(Input.Asset.AssetClassName);
		Writer.WriteString(Input.RendererClassName);
		Writer.WriteU32(Input.Output.Width);
		Writer.WriteU32(Input.Output.Height);
		Writer.WriteU32(Input.Output.ColorSpaceVersion);
		Writer.WriteU32(Input.Output.EncodingVersion);
		std::vector<const FPackageData*> Packages;
		for (const auto& Package : Input.Packages) Packages.push_back(&Package);
		std::ranges::sort(Packages, {}, [](const auto* Package) { return Package->PackagePath.GetView(); });
		Writer.WriteU64(Packages.size());
		for (const auto* Package : Packages)
		{
			Writer.WriteString(Package->PackagePath.GetView());
			Writer.WriteU32(Package->FormatVersion);
			Writer.WriteU64(Package->FileSize);
			Writer.WriteI64(Package->LastWriteTimeTicks);
		}
		return FXxHash128::HashBuffer(Writer.GetBytes()).ToString();
	}
}
