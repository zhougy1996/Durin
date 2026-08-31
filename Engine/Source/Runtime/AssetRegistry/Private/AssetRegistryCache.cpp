#include "AssetRegistryCacheInternal.h"
#include "AssetRegistry/PackageFormat.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Asset::Private
{
	namespace
	{
		constexpr uint32 AssetRegistryMagic = 0x47455241; // AREG
		constexpr uint32 AssetRegistrySchemaVersion = 3;
		constexpr uint64 MaximumRegistryEntries = 1000000;
		constexpr uint32 MaximumRegistryDependencies = 100000;
		constexpr std::string_view RedirectorClassName =
			"Durin::Asset::DAssetRedirector";

		auto IsValidRegistryCacheHeader(
			const FRegistryCacheEntry& Entry) -> bool
		{
			const auto PathsCanonical = [](const std::vector<FAssetPath>& Paths)
			{
				return std::ranges::is_sorted(Paths, [](const FAssetPath& A, const FAssetPath& B)
					{ return A.GetView() < B.GetView(); })
					&& std::adjacent_find(Paths.begin(), Paths.end()) == Paths.end();
			};
			if (Entry.ObjectCount == 0 || !PathsCanonical(Entry.Dependencies)
				|| !PathsCanonical(Entry.SoftDependencies)
				|| !std::ranges::is_sorted(Entry.SearchableNames)
				|| std::adjacent_find(Entry.SearchableNames.begin(), Entry.SearchableNames.end())
					!= Entry.SearchableNames.end()) return false;
			if (Entry.EntryKind == EAssetRegistryEntryKind::Asset)
				return !Entry.RedirectDestination.IsValid()
					&& Entry.AssetClassName != RedirectorClassName;
			return Entry.EntryKind == EAssetRegistryEntryKind::Redirector
				&& Entry.AssetClassName == RedirectorClassName
				&& Entry.RedirectDestination.IsValid()
				&& Entry.Dependencies.size() == 1
				&& Entry.Dependencies.front() == Entry.RedirectDestination;
		}
	}


	auto RegistryCachePath() -> std::filesystem::path
	{
		return std::filesystem::path(FPaths::DerivedDataCacheDir()) / "AssetRegistry" / "Registry.bin";
	}

	auto GetMountManifest() -> std::vector<std::string>
		{
			std::vector<std::string> Roots;
			for (const FMountPoint& Mount : FMountPaths::GetRegisteredMountPoints())
				if (Mount.bAutoScan) Roots.push_back(Mount.VirtualRoot);
		std::ranges::sort(Roots);
		Roots.erase(std::unique(Roots.begin(), Roots.end()), Roots.end());
		return Roots;
	}

	auto MakeRegistryIdentity(std::string_view MountRoot, std::string_view RelativePath) -> std::string
	{
		return std::format("{}\n{}", MountRoot, RelativePath);
	}

	auto LoadRegistryCache(const std::vector<std::string>& ExpectedMounts,
		std::unordered_map<std::string, FRegistryCacheEntry>& OutEntries, std::string& OutWarning) -> bool
	{
		OutEntries.clear();
		const std::filesystem::path Path = RegistryCachePath();
		std::error_code Ec;
		if (!std::filesystem::exists(Path, Ec)) return false;
		const uintmax_t Size = std::filesystem::file_size(Path, Ec);
		if (Ec || Size > 256ull * 1024ull * 1024ull)
		{
			OutWarning = std::format("Ignoring invalid asset registry cache {}.", Path.generic_string());
			return false;
		}
		std::vector<std::byte> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, Path))
		{
			OutWarning = std::format("Failed to read asset registry cache {}.", Path.generic_string());
			return false;
		}
		FBinaryReader Reader(Bytes);
		uint32 MountCount = 0;
		if (!Reader.ReadAndValidateHeader(AssetRegistryMagic, AssetRegistrySchemaVersion, AssetPackageReaderPolicyFingerprint)
			|| !Reader.ReadU32(MountCount) || MountCount > MaximumRegistryEntries)
		{
			OutWarning = "Ignoring incompatible or corrupt asset registry cache header.";
			return false;
		}
		std::vector<std::string> Mounts;
		Mounts.reserve(MountCount);
		for (uint32 Index = 0; Index < MountCount; ++Index)
		{
			std::string Root;
			if (!Reader.ReadString(Root)) { OutWarning = "Ignoring truncated asset registry mount manifest."; return false; }
			Mounts.push_back(std::move(Root));
		}
		if (Mounts != ExpectedMounts)
		{
			OutWarning = "Ignoring asset registry cache because the mount manifest changed.";
			return false;
		}
		uint64 EntryCount = 0;
		if (!Reader.ReadU64(EntryCount) || EntryCount > MaximumRegistryEntries)
		{
			OutWarning = "Ignoring invalid asset registry cache entry count.";
			return false;
		}
		for (uint64 Index = 0; Index < EntryCount; ++Index)
		{
			FRegistryCacheEntry Entry;
			uint8 EntryKind = 0;
			std::string RedirectDestination;
			uint32 DependencyCount = 0;
			if (!Reader.ReadString(Entry.MountRoot) || !Reader.ReadString(Entry.RelativePath)
				|| !Reader.ReadString(Entry.AssetClassName)
				|| !Reader.ReadU8(EntryKind)
				|| EntryKind > uint8(EAssetRegistryEntryKind::Redirector)
				|| !Reader.ReadString(RedirectDestination)
				|| !Reader.ReadU32(Entry.FormatVersion)
				|| !Reader.ReadU32(DependencyCount) || DependencyCount > MaximumRegistryDependencies)
			{
				OutWarning = "Ignoring corrupt asset registry cache entry.";
				OutEntries.clear();
				return false;
			}
			Entry.EntryKind = static_cast<EAssetRegistryEntryKind>(EntryKind);
			if (!RedirectDestination.empty()
				&& !FAssetPath::TryCreate(
					RedirectDestination, Entry.RedirectDestination))
			{
				OutWarning = "Ignoring invalid redirect destination in asset registry cache.";
				OutEntries.clear();
				return false;
			}
			Entry.Dependencies.reserve(DependencyCount);
			for (uint32 DependencyIndex = 0; DependencyIndex < DependencyCount; ++DependencyIndex)
			{
				std::string DependencyString;
				FAssetPath Dependency;
				if (!Reader.ReadString(DependencyString) || !FAssetPath::TryCreate(DependencyString, Dependency))
				{
					OutWarning = "Ignoring invalid dependency in asset registry cache.";
					OutEntries.clear();
					return false;
				}
				Entry.Dependencies.push_back(std::move(Dependency));
			}
			uint32 SoftCount = 0;
			if (!Reader.ReadU32(SoftCount) || SoftCount > MaximumRegistryDependencies)
			{
				OutWarning = "Ignoring invalid soft dependency count in asset registry cache.";
				OutEntries.clear();
				return false;
			}
			for (uint32 DependencyIndex = 0; DependencyIndex < SoftCount; ++DependencyIndex)
			{
				std::string DependencyString;
				FAssetPath Dependency;
				if (!Reader.ReadString(DependencyString) || !FAssetPath::TryCreate(DependencyString, Dependency))
				{
					OutWarning = "Ignoring invalid soft dependency in asset registry cache.";
					OutEntries.clear();
					return false;
				}
				Entry.SoftDependencies.push_back(std::move(Dependency));
			}
			uint32 SearchableCount = 0;
			if (!Reader.ReadU32(SearchableCount) || SearchableCount > MaximumRegistryDependencies)
			{
				OutWarning = "Ignoring invalid searchable-name count in asset registry cache.";
				OutEntries.clear();
				return false;
			}
			for (uint32 SearchableIndex = 0; SearchableIndex < SearchableCount; ++SearchableIndex)
			{
				std::string Name;
				if (!Reader.ReadString(Name) || Name.empty())
				{
					OutWarning = "Ignoring invalid searchable name in asset registry cache.";
					OutEntries.clear();
					return false;
				}
				Entry.SearchableNames.push_back(std::move(Name));
			}
			if (!Reader.ReadU64(Entry.ObjectCount) || !Reader.ReadU64(Entry.BulkSegmentExtent)
				|| !Reader.ReadHash128(Entry.BulkSegmentDigest)
				|| ((Entry.BulkSegmentExtent == 0) != Entry.BulkSegmentDigest.IsZero())
				|| !Reader.ReadU64(Entry.FileSize) || !Reader.ReadI64(Entry.LastWriteTimeTicks)
				|| !IsSupportedAssetPackageReaderVersion(Entry.FormatVersion)
				|| !std::ranges::binary_search(ExpectedMounts, Entry.MountRoot)
				|| std::filesystem::path(Entry.RelativePath).is_absolute()
				|| std::filesystem::path(Entry.RelativePath).extension() != ".dasset"
				|| Entry.RelativePath.starts_with("../") || Entry.RelativePath.find("/../") != std::string::npos)
			{
				OutWarning = "Ignoring invalid asset registry cache identity.";
				OutEntries.clear();
				return false;
			}
			if (!IsValidRegistryCacheHeader(Entry))
			{
				OutWarning = "Ignoring corrupt redirect metadata in asset registry cache.";
				OutEntries.clear();
				return false;
			}
			const std::string Identity = MakeRegistryIdentity(Entry.MountRoot, Entry.RelativePath);
			if (!OutEntries.emplace(Identity, std::move(Entry)).second)
			{
				OutWarning = "Ignoring duplicate asset registry cache identity.";
				OutEntries.clear();
				return false;
			}
		}
		if (!Reader.IsAtEnd())
		{
			OutWarning = "Ignoring asset registry cache with trailing data.";
			OutEntries.clear();
			return false;
		}
		return true;
	}

	auto WriteRegistryCache(const std::vector<std::string>& Mounts, std::vector<FRegistryCacheEntry> Entries,
		std::string& OutWarning) -> bool
	{
		std::ranges::sort(Entries, [](const FRegistryCacheEntry& A, const FRegistryCacheEntry& B) {
			return std::tie(A.MountRoot, A.RelativePath) < std::tie(B.MountRoot, B.RelativePath);
		});
		FBinaryWriter Writer;
		Writer.WriteHeader({AssetRegistryMagic, AssetRegistrySchemaVersion, AssetPackageReaderPolicyFingerprint});
		Writer.WriteU32(static_cast<uint32>(Mounts.size()));
		for (const std::string& Mount : Mounts) Writer.WriteString(Mount);
		Writer.WriteU64(Entries.size());
		for (const FRegistryCacheEntry& Entry : Entries)
		{
			Writer.WriteString(Entry.MountRoot);
			Writer.WriteString(Entry.RelativePath);
			Writer.WriteString(Entry.AssetClassName);
			Writer.WriteU8(static_cast<uint8>(Entry.EntryKind));
			Writer.WriteString(Entry.RedirectDestination.GetView());
			Writer.WriteU32(Entry.FormatVersion);
			Writer.WriteU32(static_cast<uint32>(Entry.Dependencies.size()));
			for (const FAssetPath& Dependency : Entry.Dependencies) Writer.WriteString(Dependency.GetView());
			Writer.WriteU32(static_cast<uint32>(Entry.SoftDependencies.size()));
			for (const FAssetPath& Dependency : Entry.SoftDependencies) Writer.WriteString(Dependency.GetView());
			Writer.WriteU32(static_cast<uint32>(Entry.SearchableNames.size()));
			for (const std::string& Name : Entry.SearchableNames) Writer.WriteString(Name);
			Writer.WriteU64(Entry.ObjectCount);
			Writer.WriteU64(Entry.BulkSegmentExtent);
			Writer.WriteHash128(Entry.BulkSegmentDigest);
			Writer.WriteU64(Entry.FileSize);
			Writer.WriteI64(Entry.LastWriteTimeTicks);
		}
		FFileHelper::FAtomicFileError FileError;
		if (!FFileHelper::SaveArrayToFileAtomically(Writer.GetBytes(), RegistryCachePath(), &FileError))
		{
			OutWarning = FileError.ToString();
			return false;
		}
		return true;
	}

	auto BuildRegistryCacheEntries(const std::unordered_map<FAssetPath, FAssetData>& Assets,
		std::vector<FRegistryCacheEntry>& OutEntries, std::string& OutWarning) -> bool
	{
		OutEntries.clear();
		OutEntries.reserve(Assets.size());
		for (const auto& [Path, Data] : Assets)
		{
			const FMountLookupResult Lookup =
				FMountPaths::FindMountForVirtualPath(Path.GetView());
			if (!Lookup || !Lookup.Mount->bAutoScan)
			{
				OutWarning = std::format("Could not persist asset registry entry {} because its mount is unavailable.", Path.ToString());
				OutEntries.clear();
				return false;
			}
			const std::string RelativeAssetPath = Lookup.RelativePath.generic_string();
			const std::string RelativeString = std::format("{}.dasset", RelativeAssetPath);
			if (RelativeAssetPath.empty() || std::filesystem::path(RelativeString).is_absolute()
				|| RelativeString.starts_with("../") || RelativeString.find("/../") != std::string::npos)
			{
				OutWarning = std::format("Could not persist invalid asset registry path {}.", Path.ToString());
				OutEntries.clear();
				return false;
			}
			OutEntries.push_back(FRegistryCacheEntry{
				.MountRoot = Lookup.Mount->VirtualRoot,
				.RelativePath = RelativeString,
				.AssetClassName = Data.AssetClassName,
				.EntryKind = Data.EntryKind,
				.RedirectDestination = Data.RedirectDestination,
				.FormatVersion = Data.FormatVersion,
				.Dependencies = Data.Dependencies,
				.SoftDependencies = Data.SoftDependencies,
				.SearchableNames = Data.SearchableNames,
				.ObjectCount = Data.ObjectCount,
				.BulkSegmentExtent = Data.BulkSegmentExtent,
				.BulkSegmentDigest = Data.BulkSegmentDigest,
				.FileSize = Data.FileSize,
				.LastWriteTimeTicks = Data.LastWriteTimeTicks});
		}
		return true;
	}

}
