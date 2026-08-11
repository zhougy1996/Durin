#include "Thumbnail/AssetThumbnailKey.h"

#include "Hash/XxHash.h"

namespace Durin::Editor
{
	namespace
	{
		class FThumbnailKeyWriter
		{
		public:
			auto WriteU32(uint32 Value) -> void
			{
				for (uint32 ByteIndex = 0; ByteIndex < 4; ++ByteIndex)
					Bytes.push_back(static_cast<uint8>(Value >> (ByteIndex * 8)));
			}

			auto WriteU64(uint64 Value) -> void
			{
				for (uint32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
					Bytes.push_back(static_cast<uint8>(Value >> (ByteIndex * 8)));
			}

			auto WriteI64(int64 Value) -> void
			{
				WriteU64(std::bit_cast<uint64>(Value));
			}

			auto WriteString(std::string_view Value) -> void
			{
				WriteU64(static_cast<uint64>(Value.size()));
				Bytes.insert(Bytes.end(), Value.begin(), Value.end());
			}

			auto GetBytes() const -> std::span<const uint8>
			{
				return Bytes;
			}

		private:
			std::vector<uint8> Bytes;
		};

		auto WritePackageFingerprint(FThumbnailKeyWriter& Writer, const FAssetThumbnailPackageFingerprint& Package) -> void
		{
			Writer.WriteString(Package.VirtualPath.GetView());
			Writer.WriteString(Package.AssetClassName);
			Writer.WriteU32(Package.PackageFormatVersion);
			Writer.WriteU64(Package.FileSize);
			Writer.WriteI64(Package.LastWriteTimeTicks);
		}
	} // namespace
	auto BuildAssetThumbnailDependencyClosure(
		const FAssetPath& Root,
		std::span<const FAssetThumbnailDependencyNode> RegistrySnapshot,
		std::vector<FAssetThumbnailPackageFingerprint>& OutDependencies,
		std::string& OutError) -> bool
	{
		OutDependencies.clear();
		OutError.clear();
		if (!Root.IsValid())
		{
			OutError = "The thumbnail dependency root is invalid.";
			return false;
		}

		std::unordered_map<std::string_view, const FAssetThumbnailDependencyNode*> Nodes;
		Nodes.reserve(RegistrySnapshot.size());
		for (const FAssetThumbnailDependencyNode& Node : RegistrySnapshot)
		{
			if (!Node.Package.VirtualPath.IsValid())
			{
				OutError = "The Asset Registry snapshot contains an invalid package path.";
				return false;
			}
			const auto [It, bInserted] = Nodes.emplace(Node.Package.VirtualPath.GetView(), &Node);
			if (!bInserted)
			{
				OutError = std::format("The Asset Registry snapshot contains a duplicate entry for '{}'.",
					Node.Package.VirtualPath.GetView());
				return false;
			}
		}

		const auto RootIt = Nodes.find(Root.GetView());
		if (RootIt == Nodes.end())
		{
			OutError = std::format("The Asset Registry has no entry for thumbnail root '{}'.", Root.GetView());
			return false;
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
			for (const FAssetPath& Dependency : Node->Dependencies)
				SortedDependencies.push_back(Dependency.GetView());
			std::ranges::sort(SortedDependencies);

			for (const std::string_view DependencyPath : SortedDependencies)
			{
				if (!Visited.emplace(DependencyPath).second) continue;
				const auto DependencyIt = Nodes.find(DependencyPath);
				if (DependencyIt == Nodes.end())
				{
					OutDependencies.clear();
					OutError = std::format("The Asset Registry has no entry for thumbnail dependency '{}'.", DependencyPath);
					return false;
				}
				OutDependencies.push_back(DependencyIt->second->Package);
				Pending.push_back(DependencyIt->second);
			}
		}

		std::ranges::sort(OutDependencies, {}, [](const FAssetThumbnailPackageFingerprint& Package) {
			return Package.VirtualPath.GetView();
		});
		return true;
	}

	auto BuildAssetThumbnailCacheKey(const FAssetThumbnailKeyInput& Input) -> std::string
	{
		FThumbnailKeyWriter Writer;
		Writer.WriteString("DurinAssetThumbnailKey");
		Writer.WriteU32(1);
		WritePackageFingerprint(Writer, Input.Asset);
		Writer.WriteString(Input.ProviderName);
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
			return Package.VirtualPath.GetView();
		});
		Writer.WriteU64(static_cast<uint64>(Dependencies.size()));
		for (const FAssetThumbnailPackageFingerprint& Dependency : Dependencies)
			WritePackageFingerprint(Writer, Dependency);
		return FXxHash128::HashBuffer(Writer.GetBytes()).ToString();
	}
} // namespace Durin::Editor
