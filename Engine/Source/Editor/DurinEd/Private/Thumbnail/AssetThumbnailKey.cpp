#include "Thumbnail/AssetThumbnailKey.h"

#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Editor
{
	namespace
	{
		auto WritePackageFingerprint(FBinaryWriter& Writer, const FAssetThumbnailPackageFingerprint& Package) -> void
		{
			Writer.WriteString(Package.AssetPath.ToString());
			Writer.WriteString(Package.PackagePath.GetView());
			Writer.WriteString(Package.AssetClassName);
			Writer.WriteU32(Package.PackageFormatVersion);
			Writer.WriteU64(Package.FileSize);
			Writer.WriteI64(Package.LastWriteTimeTicks);
		}
	} // namespace
	auto BuildAssetThumbnailDependencyClosure(
		const FPackagePath& Root,
		std::span<const FAssetThumbnailDependencyNode> RegistrySnapshot)
		-> std::expected<std::vector<FAssetThumbnailPackageFingerprint>, std::string>
	{
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;

		if (!Root.IsValid())
		{
			return std::unexpected("The thumbnail dependency root is invalid.");
		}

		std::unordered_map<std::string_view, const FAssetThumbnailDependencyNode*> Nodes;
		Nodes.reserve(RegistrySnapshot.size());
		for (const FAssetThumbnailDependencyNode& Node : RegistrySnapshot)
		{
			if (!Node.Package.PackagePath.IsValid())
			{
				return std::unexpected("The Asset Registry snapshot contains an invalid package path.");
			}
			const auto [It, bInserted] = Nodes.emplace(Node.Package.PackagePath.GetView(), &Node);
			if (!bInserted)
			{
				return std::unexpected(std::format("The Asset Registry snapshot contains a duplicate entry for '{}'.",
					Node.Package.PackagePath.GetView()));
			}
		}

		const auto RootIt = Nodes.find(Root.GetView());
		if (RootIt == Nodes.end())
		{
			return std::unexpected(std::format("The Asset Registry has no entry for thumbnail root '{}'.", Root.GetView()));
		}

		std::unordered_set<std::string_view> Visited;
		std::vector<const FAssetThumbnailDependencyNode*> Pending{RootIt->second};
		Visited.emplace(Root.GetView());
		while (!Pending.empty())
		{
			const FAssetThumbnailDependencyNode* Node = Pending.back();
			Pending.pop_back();

			std::vector<std::string_view> SortedDependencies;
			SortedDependencies.reserve(Node->Dependencies.size());
			for (const FPackagePath& Dependency : Node->Dependencies)
				SortedDependencies.push_back(Dependency.GetView());
			std::ranges::sort(SortedDependencies);

			for (const std::string_view DependencyPath : SortedDependencies)
			{
				if (!Visited.emplace(DependencyPath).second) continue;
				const auto DependencyIt = Nodes.find(DependencyPath);
				if (DependencyIt == Nodes.end())
				{
					return std::unexpected(std::format("The Asset Registry has no entry for thumbnail dependency '{}'.", DependencyPath));
				}
				Dependencies.push_back(DependencyIt->second->Package);
				Pending.push_back(DependencyIt->second);
			}
		}

		std::ranges::sort(Dependencies, {}, [](const FAssetThumbnailPackageFingerprint& Package) {
			return Package.PackagePath.GetView();
		});
		return Dependencies;
	}

	auto BuildAssetThumbnailCacheKey(const FAssetThumbnailKeyInput& Input) -> std::string
	{
		FBinaryWriter Writer;
		Writer.WriteString("DurinAssetThumbnailKey");
		Writer.WriteU32(2);
		WritePackageFingerprint(Writer, Input.Asset);
		Writer.WriteString(Input.RendererName);
		Writer.WriteU32(Input.GeneratorSchemaVersion);
		Writer.WriteU32(Input.Output.Width);
		Writer.WriteU32(Input.Output.Height);
		Writer.WriteU32(Input.Output.ColorSpaceVersion);
		Writer.WriteU32(Input.Output.EncodingVersion);
		Writer.WriteString(Input.PreviewFixtureIdentity);
		Writer.WriteU32(Input.PreviewFixtureVersion);
		Writer.WriteU32(Input.ShaderContractVersion);

		std::vector<FAssetThumbnailPackageFingerprint> Dependencies = Input.Dependencies;
		std::ranges::sort(Dependencies, {}, [](const FAssetThumbnailPackageFingerprint& Package) {
			return Package.PackagePath.GetView();
		});
		Writer.WriteU64(static_cast<uint64>(Dependencies.size()));
		for (const FAssetThumbnailPackageFingerprint& Dependency : Dependencies)
			WritePackageFingerprint(Writer, Dependency);
		return FXxHash128::HashBuffer(Writer.GetBytes()).ToString();
	}
} // namespace Durin::Editor
