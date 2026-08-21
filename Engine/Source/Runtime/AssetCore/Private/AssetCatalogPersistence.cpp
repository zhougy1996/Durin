#include "AssetCatalogPersistenceInternal.h"
#include "AssetPackageValueCodec.h"
#include "AssetPackageVersionPolicy.h"

#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::Asset::Private
{
	namespace
	{
		constexpr uint64 MaximumRegistryEntries = 1000000;
		constexpr uint32 MaximumRegistryDependencies = 100000;
		constexpr uint64 MaximumReferencesPerPackage = 100000;
		constexpr uint64 MaximumReferencesPerSnapshot = 1000000;
		constexpr uint64 MaximumReferenceRouteTokenBytes = 1024 * 1024;
		constexpr uint32 MaximumReferenceContainerDepth = 4;
		constexpr uint64 MaximumReferenceDisplayRouteBytes = 4 * 1024;
		constexpr std::string_view RedirectorClassName =
			"Durin::Asset::DAssetRedirector";

		auto IsValidRegistryCacheHeader(
			const FRegistryCacheEntry& Entry) -> bool
		{
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
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
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
		std::vector<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
		{
			OutWarning = std::format("Failed to read asset registry cache {}.", Path.generic_string());
			return false;
		}
		DerivedDataCache::FReader Reader(Bytes);
		uint32 MountCount = 0;
		if (!Reader.ReadAndValidateHeader(DerivedDataCache::AssetRegistryMagic, DerivedDataCache::AssetRegistrySchemaVersion, AssetPackageReaderPolicyFingerprint)
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
			if (!Reader.ReadU64(Entry.FileSize) || !Reader.ReadI64(Entry.LastWriteTimeTicks)
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
		DerivedDataCache::FWriter Writer;
		Writer.WriteHeader({DerivedDataCache::AssetRegistryMagic, DerivedDataCache::AssetRegistrySchemaVersion, AssetPackageReaderPolicyFingerprint});
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
			Writer.WriteU64(Entry.FileSize);
			Writer.WriteI64(Entry.LastWriteTimeTicks);
		}
		std::string ErrorMessage;
		if (!DerivedDataCache::WriteFileAtomically(RegistryCachePath(), Writer.GetBytes(), &ErrorMessage))
		{
			OutWarning = std::move(ErrorMessage);
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
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(Path.GetView());
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
				.FileSize = Data.FileSize,
				.LastWriteTimeTicks = Data.LastWriteTimeTicks});
		}
		return true;
	}

	constexpr uint32 AssetReferenceIndexMagic = 0x58495241; // ARIX
	constexpr uint32 AssetReferenceIndexSchemaVersion = 1;
	constexpr uint32 AssetReferenceExtractorSchemaVersion = 1;
	constexpr uintmax_t MaximumReferenceCacheBytes = 1024ull * 1024ull * 1024ull;


	auto ReferenceCachePath() -> std::filesystem::path
	{
		return std::filesystem::path(FPaths::DerivedDataCacheDir())
			/ "AssetRegistry" / "References.bin";
	}

	auto LoadReferenceCache(
		std::unordered_map<FAssetPath, FReferenceCacheSource>& OutSources,
		std::string& OutWarning) -> bool
	{
		OutSources.clear();
		const std::filesystem::path Path = ReferenceCachePath();
		std::error_code Ec;
		if (!std::filesystem::exists(Path, Ec)) return false;
		const uintmax_t Size = std::filesystem::file_size(Path, Ec);
		if (Ec || Size > MaximumReferenceCacheBytes)
		{
			OutWarning = std::format("Ignoring invalid asset-reference cache {}.", Path.generic_string());
			return false;
		}
		std::vector<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
		{
			OutWarning = std::format("Failed to read asset-reference cache {}.", Path.generic_string());
			return false;
		}
		DerivedDataCache::FReader Reader(Bytes);
		uint32 ExtractorSchema = 0;
		uint64 SourceCount = 0;
		if (!Reader.ReadAndValidateHeader(
				AssetReferenceIndexMagic, AssetReferenceIndexSchemaVersion, AssetPackageReaderPolicyFingerprint)
			|| !Reader.ReadU32(ExtractorSchema)
			|| ExtractorSchema != AssetReferenceExtractorSchemaVersion
			|| !Reader.ReadU64(SourceCount) || SourceCount > MaximumRegistryEntries)
		{
			OutWarning = "Ignoring incompatible or corrupt asset-reference cache header.";
			return false;
		}
		uint64 TotalOccurrences = 0;
		for (uint64 SourceIndex = 0; SourceIndex < SourceCount; ++SourceIndex)
		{
			std::string SourceString;
			FAssetPath SourcePath;
			FReferenceCacheSource Source;
			uint64 FileSize = 0;
			uint64 OccurrenceCount = 0;
			if (!Reader.ReadString(SourceString, MaximumPackageStringBytes)
				|| !FAssetPath::TryCreate(SourceString, SourcePath)
				|| !Reader.ReadU64(FileSize)
				|| !Reader.ReadI64(Source.Fingerprint.LastWriteTimeTicks)
				|| !Reader.ReadU64(Source.Fingerprint.ContentHash.HashLow)
				|| !Reader.ReadU64(Source.Fingerprint.ContentHash.HashHigh)
				|| !Reader.ReadU32(Source.Fingerprint.ReaderVersion)
				|| !IsSupportedAssetPackageReaderVersion(Source.Fingerprint.ReaderVersion)
				|| !Reader.ReadU64(OccurrenceCount)
				|| OccurrenceCount > MaximumReferencesPerPackage
				|| TotalOccurrences > MaximumReferencesPerSnapshot - OccurrenceCount)
			{
				OutWarning = "Ignoring corrupt asset-reference cache source record.";
				OutSources.clear();
				return false;
			}
			Source.Fingerprint.FileSize = static_cast<uintmax_t>(FileSize);
			TotalOccurrences += OccurrenceCount;
			Source.References.reserve(static_cast<size_t>(OccurrenceCount));
			for (uint64 OccurrenceIndex = 0; OccurrenceIndex < OccurrenceCount; ++OccurrenceIndex)
			{
				FAssetReferenceEdge Reference{
					.SourcePackage = SourcePath,
					.SourceFingerprint = Source.Fingerprint};
				std::string TargetString;
				uint32 RouteCount = 0;
				uint8 ReferenceKind = 0;
				if (!Reader.ReadU64(Reference.SourceObjectId)
					|| !Reader.ReadString(Reference.SourceClass, MaximumPackageStringBytes)
					|| !Reader.ReadString(Reference.DeclaringType, MaximumPackageStringBytes)
					|| !Reader.ReadString(Reference.FieldName, MaximumPackageStringBytes)
					|| !Reader.ReadU8(ReferenceKind)
					|| ReferenceKind > static_cast<uint8>(EAssetReferenceKind::Redirect)
					|| !Reader.ReadString(Reference.ExpectedClass, MaximumPackageStringBytes)
					|| !Reader.ReadString(TargetString, MaximumPackageStringBytes)
					|| !FAssetPath::TryCreate(TargetString, Reference.TargetPath)
					|| !Reader.ReadU32(RouteCount)
					|| RouteCount > MaximumReferenceContainerDepth)
				{
					OutWarning = "Ignoring corrupt asset-reference cache occurrence.";
					OutSources.clear();
					return false;
				}
				Reference.Kind = static_cast<EAssetReferenceKind>(ReferenceKind);
				Reference.Route.reserve(RouteCount);
				for (uint32 RouteIndex = 0; RouteIndex < RouteCount; ++RouteIndex)
				{
					uint8 Kind = 0;
					uint64 TokenBytes = 0;
					FAssetReferenceRouteSegment Segment;
					if (!Reader.ReadU8(Kind)
						|| Kind > static_cast<uint8>(EAssetReferenceRouteKind::StructField)
						|| !Reader.ReadU64(Segment.Index)
						|| !Reader.ReadU64(TokenBytes)
						|| !Reader.ReadBytes(
							Segment.MapKeyToken, TokenBytes, MaximumReferenceRouteTokenBytes)
						|| !Reader.ReadString(Segment.DeclaringType, MaximumPackageStringBytes)
						|| !Reader.ReadString(Segment.FieldName, MaximumPackageStringBytes))
					{
						OutWarning = "Ignoring corrupt asset-reference cache route.";
						OutSources.clear();
						return false;
					}
					Segment.Kind = static_cast<EAssetReferenceRouteKind>(Kind);
					if ((Segment.Kind == EAssetReferenceRouteKind::MapValue)
						!= !Segment.MapKeyToken.empty()
						|| (Segment.Kind == EAssetReferenceRouteKind::StructField)
						!= (!Segment.DeclaringType.empty() && !Segment.FieldName.empty()))
					{
						OutWarning = "Ignoring inconsistent asset-reference cache route.";
						OutSources.clear();
						return false;
					}
					Reference.Route.push_back(std::move(Segment));
				}
				if (!Reader.ReadString(
					Reference.DisplayRoute, MaximumReferenceDisplayRouteBytes)
					|| Reference.DisplayRoute.empty())
				{
					OutWarning = "Ignoring invalid asset-reference cache display route.";
					OutSources.clear();
					return false;
				}
				Source.References.push_back(std::move(Reference));
			}
			if (!OutSources.emplace(SourcePath, std::move(Source)).second)
			{
				OutWarning = "Ignoring duplicate asset-reference cache source.";
				OutSources.clear();
				return false;
			}
		}
		if (!Reader.IsAtEnd())
		{
			OutWarning = "Ignoring asset-reference cache with trailing data.";
			OutSources.clear();
			return false;
		}
		return true;
	}

	auto WriteReferenceCache(
		const std::unordered_map<FAssetPath, FAssetPackageFingerprint>& Fingerprints,
		std::span<const FAssetReferenceEdge> References,
		std::string& OutWarning) -> bool
	{
		if (Fingerprints.size() > MaximumRegistryEntries
			|| References.size() > MaximumReferencesPerSnapshot)
		{
			OutWarning = "Asset-reference index exceeds its persisted snapshot bounds.";
			return false;
		}
		std::unordered_map<FAssetPath, std::vector<const FAssetReferenceEdge*>> BySource;
		for (const FAssetReferenceEdge& Reference : References)
			BySource[Reference.SourcePackage].push_back(&Reference);
		std::vector<FAssetPath> Sources;
		Sources.reserve(Fingerprints.size());
		for (const auto& [Source, Fingerprint] : Fingerprints) Sources.push_back(Source);
		std::ranges::sort(Sources, [](const FAssetPath& Left, const FAssetPath& Right) {
			return Left.GetView() < Right.GetView();
		});

		DerivedDataCache::FWriter Writer;
		Writer.WriteHeader({AssetReferenceIndexMagic, AssetReferenceIndexSchemaVersion, AssetPackageReaderPolicyFingerprint});
		Writer.WriteU32(AssetReferenceExtractorSchemaVersion);
		Writer.WriteU64(Sources.size());
		for (const FAssetPath& Source : Sources)
		{
			const FAssetPackageFingerprint& Fingerprint = Fingerprints.at(Source);
			const auto ReferencesIt = BySource.find(Source);
			const size_t ReferenceCount = ReferencesIt == BySource.end()
				? 0 : ReferencesIt->second.size();
			if (ReferenceCount > MaximumReferencesPerPackage)
			{
				OutWarning = std::format(
					"Asset-reference source {} exceeds its occurrence bound.", Source.ToString());
				return false;
			}
			Writer.WriteString(Source.GetView());
			Writer.WriteU64(static_cast<uint64>(Fingerprint.FileSize));
			Writer.WriteI64(Fingerprint.LastWriteTimeTicks);
			Writer.WriteU64(Fingerprint.ContentHash.HashLow);
			Writer.WriteU64(Fingerprint.ContentHash.HashHigh);
			Writer.WriteU32(Fingerprint.ReaderVersion);
			Writer.WriteU64(ReferenceCount);
			if (ReferencesIt == BySource.end()) continue;
			for (const FAssetReferenceEdge* Reference : ReferencesIt->second)
			{
				Writer.WriteU64(Reference->SourceObjectId);
				Writer.WriteString(Reference->SourceClass);
				Writer.WriteString(Reference->DeclaringType);
				Writer.WriteString(Reference->FieldName);
				Writer.WriteU8(static_cast<uint8>(Reference->Kind));
				Writer.WriteString(Reference->ExpectedClass);
				Writer.WriteString(Reference->TargetPath.GetView());
				Writer.WriteU32(static_cast<uint32>(Reference->Route.size()));
				for (const FAssetReferenceRouteSegment& Segment : Reference->Route)
				{
					Writer.WriteU8(static_cast<uint8>(Segment.Kind));
					Writer.WriteU64(Segment.Index);
					Writer.WriteU64(Segment.MapKeyToken.size());
					Writer.WriteBytes(Segment.MapKeyToken);
					Writer.WriteString(Segment.DeclaringType);
					Writer.WriteString(Segment.FieldName);
				}
				Writer.WriteString(Reference->DisplayRoute);
			}
		}
		std::string ErrorMessage;
		if (!DerivedDataCache::WriteFileAtomically(
			ReferenceCachePath(), Writer.GetBytes(), &ErrorMessage))
		{
			OutWarning = std::move(ErrorMessage);
			return false;
		}
		return true;
	}
}
