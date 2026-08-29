#include "AssetRegistry/Scan.h"
#include "AssetRegistry/ObjectStream.h"
#include "AssetPublicationCoordinatorInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetMutationRegistryInternal.h"

#include "DObject/Class.h"
#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Profiling/Profiling.h"

namespace Durin::Asset
{
	using Private::AssetReferenceLess;
	using Private::FMutationPackageMetadata;
	using Private::GetAssetReferenceStoreRegistry;
	using Private::ValidateMutationPackageMetadata;

	namespace
	{
		constexpr size_t MaximumReferencesPerSnapshot = 1000000;
		constexpr uint32 MaximumRedirectDepth = 64;
		constexpr std::string_view RedirectorClassName =
			"Durin::Asset::DAssetRedirector";

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto IsMissingPathError(const std::error_code& ErrorCode) -> bool
		{
			return ErrorCode == std::errc::no_such_file_or_directory
				|| ErrorCode.value() == 2
				|| ErrorCode.value() == 3;
		}

		auto AssetPathResolutionError(
			const FAssetPathResolveResult& Resolution) -> FAssetResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::NotFound:
				return Error(EAssetError::NotFound, std::format(
					"Asset {} is not present in the registry.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::MissingRedirectTarget:
				return Error(EAssetError::NotFound, std::format(
					"Asset redirect {} has a missing target {}.",
					Resolution.RequestedPath.ToString(),
					Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectCycle:
				return Error(EAssetError::CircularDependency, std::format(
					"Asset redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(),
					Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectDepthExceeded:
				return Error(EAssetError::CircularDependency, std::format(
					"Asset redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(),
					Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::UnknownTargetClass:
				return Error(EAssetError::UnknownClass, std::format(
					"Asset {} resolves to a target with an unavailable reflected class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::RedirectTypeMismatch:
				return Error(EAssetError::TypeMismatch, std::format(
					"Asset {} resolves to a target with an incompatible class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::CorruptRedirector:
				return Error(EAssetError::CorruptFile, std::format(
					"CorruptRedirector: asset {} traverses invalid redirect metadata at {}.",
					Resolution.RequestedPath.ToString(),
					Resolution.FinalPath.ToString()));
			}
			return Error(
				EAssetError::CorruptFile,
				"Asset resolution returned an unknown state.");
		}
	}


	auto BuildCookReachability(
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages) -> FAssetResult
	{
		OutPackages.clear();
		const FAssetRegistrySnapshot RegistrySnapshot = CaptureAssetRegistrySnapshot();
		const FAssetCatalogSnapshot& Catalog = RegistrySnapshot.Catalog;
		const FAssetReferenceIndex& ReferenceIndex = RegistrySnapshot.References;
		struct FPendingCookPath
		{
			FAssetPath Path;
			std::string ExpectedClass;
			std::string Source;
		};
		std::vector<FPendingCookPath> Pending;
		Pending.reserve(Roots.size());
		for (const FAssetPath& Root : Roots)
			Pending.push_back({Root, {}, "explicit Cook root"});
		for (const auto& [Handle, Entry] : GetAssetReferenceStoreRegistry().Stores)
		{
			(void)Handle;
			IAssetReferenceStore* Store = Entry.Store;
			if (!Store) continue;
			auto Call = Entry.OwnerGate.TryEnter();
			if (Entry.OwnerGate.IsValid() && !Call) continue;
			FAssetReferenceStoreSnapshot Snapshot;
			FAssetResult StoreResult = Store->CaptureSnapshot(Snapshot);
			if (!StoreResult)
			{
				StoreResult.Message = std::format(
					"CookReachabilityExternalRootProviderFailed: {}",
					StoreResult.Message);
				return StoreResult;
			}
			for (const FAssetReferenceStoreOccurrence& Occurrence :
				Snapshot.Occurrences)
				if (Occurrence.bCookRoot)
					Pending.push_back({
						Occurrence.TargetPath,
						Occurrence.ExpectedClass,
						Occurrence.DisplayRoute});
		}
		std::unordered_set<FAssetPath> Visited;
		while (!Pending.empty())
		{
			std::ranges::sort(Pending, [](const FPendingCookPath& Left,
				const FPendingCookPath& Right) {
				return Left.Path.GetView() > Right.Path.GetView();
			});
			FPendingCookPath Requested = std::move(Pending.back());
			Pending.pop_back();
			DClass* ExpectedClass = nullptr;
			if (!Requested.ExpectedClass.empty())
			{
				ExpectedClass = FindClassByQualifiedName(FName(Requested.ExpectedClass));
				if (!ExpectedClass)
					return Error(EAssetError::UnknownClass, std::format(
						"CookReachabilityUnknownRootClass: {} expects unavailable class {}.",
						Requested.Source, Requested.ExpectedClass));
			}
			const FAssetPathResolveResult SourceResolution = RegistrySnapshot.ResolveAssetPath(
				Requested.Path, {.ExpectedClass = ExpectedClass});
			if (!SourceResolution)
			{
				FAssetResult ResolutionError = AssetPathResolutionError(SourceResolution);
				if (ResolutionError.Error == EAssetError::NotFound)
					ResolutionError.Error = EAssetError::MissingDependency;
				ResolutionError.Message = std::format(
					"CookReachabilityUnresolvedRoot: {} from {}. {}",
					Requested.Path.ToString(), Requested.Source,
					ResolutionError.Message);
				return ResolutionError;
			}
			const FAssetPath Source = SourceResolution.FinalPath;
			if (!Visited.insert(Source).second) continue;
			const FAssetData* SourceData = Catalog.FindExact(Source);
			if (!SourceData || SourceData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetError::InvalidPackageType, std::format(
					"CookReachabilityNonAssetPackage: {} is not a real asset.", Source.ToString()));
			if (!ReferenceIndex.GetSourceFingerprints().contains(Source))
				return Error(EAssetError::StaleData, std::format(
					"CookReachabilityIncompleteReferenceIndex: {} has no current source entry.", Source.ToString()));
			for (const FAssetPath& Dependency : SourceData->Dependencies)
			{
				const FAssetPathResolveResult Resolution =
					RegistrySnapshot.ResolveAssetPath(Dependency);
				if (!Resolution)
				{
					FAssetResult ResolutionError = AssetPathResolutionError(Resolution);
					if (ResolutionError.Error == EAssetError::NotFound)
						ResolutionError.Error = EAssetError::MissingDependency;
					ResolutionError.Message = std::format(
						"CookReachabilityUnresolvedHardDependency: {} references {}. {}",
						Source.ToString(), Dependency.ToString(), ResolutionError.Message);
					return ResolutionError;
				}
				Pending.push_back({
					Resolution.FinalPath, {},
					std::format("hard dependency of {}", Source.ToString())});
			}
			for (const FAssetReferenceEdge& Reference : ReferenceIndex.GetEdges())
			{
				if (Reference.SourcePackage != Source
					|| Reference.Kind == EAssetReferenceKind::Redirect) continue;
				DClass* ExpectedClass = FindClassByQualifiedName(FName(Reference.ExpectedClass));
				if (!ExpectedClass)
					return Error(EAssetError::UnknownClass, std::format(
						"CookReachabilityUnknownReferenceClass: {} expects unavailable class {}.",
						Reference.DisplayRoute, Reference.ExpectedClass));
				const FAssetPathResolveResult Resolution = RegistrySnapshot.ResolveAssetPath(
					Reference.TargetPath, {.ExpectedClass = ExpectedClass});
				if (!Resolution)
				{
					FAssetResult ResolutionError = AssetPathResolutionError(Resolution);
					if (ResolutionError.Error == EAssetError::NotFound)
						ResolutionError.Error = EAssetError::MissingDependency;
					ResolutionError.Message = std::format(
						"CookReachabilityUnresolvedReference: {} references {} at {}. {}",
						Source.ToString(), Reference.TargetPath.ToString(),
						Reference.DisplayRoute, ResolutionError.Message);
					return ResolutionError;
				}
				Pending.push_back({
					Resolution.FinalPath, {}, Reference.DisplayRoute});
			}
		}
		OutPackages.assign(Visited.begin(), Visited.end());
		std::ranges::sort(OutPackages, [](const FAssetPath& Left, const FAssetPath& Right) {
			return Left.GetView() < Right.GetView();
		});
		return {};
	}

	auto FAssetPublicationCoordinator::PublishAssetMetadata(FAssetData Data)
		-> FAssetResult
	{
		std::vector<FAssetData> Batch;
		Batch.push_back(std::move(Data));
		return PublishAssetMetadataBatch(std::move(Batch));
	}

	auto FAssetPublicationCoordinator::PublishAssetMetadataBatch(
		std::vector<FAssetData> Assets) -> FAssetResult
	{
		if (Assets.empty()) return {};
		FAssetRegistryPublication Publication = CaptureAssetRegistryPublication();
		const uint64 ExpectedRevision = Publication.ExpectedRevision;
		if (!Publication.bReferenceIndexComplete
			|| !Publication.ReferenceErrors.empty())
			return Error(EAssetError::StaleData,
				"Asset metadata cannot publish while the reference index is incomplete.");

		std::unordered_set<FAssetPath> SeenPaths;
		std::vector<FAssetPath> Paths;
		Paths.reserve(Assets.size());
		for (const FAssetData& Data : Assets)
		{
			if (!Data.PackagePath.IsValid()
				|| !SeenPaths.insert(Data.PackagePath).second)
				return Error(EAssetError::InvalidPath,
					"Asset metadata batch contains an invalid or duplicate package path.");
			Paths.push_back(Data.PackagePath);
		}
		for (FAssetData& Data : Assets)
		{
			const FAssetPath Path = Data.PackagePath;
			Publication.Assets.insert_or_assign(Path, std::move(Data));
			Publication.ReferenceFingerprints.erase(Path);
			std::erase_if(Publication.ReferenceEdges,
				[&](const FAssetReferenceEdge& Edge) {
					return Edge.SourcePackage == Path;
				});
		}

		for (const FAssetPath& Path : Paths)
		{
			const FAssetData& Stored = Publication.Assets.at(Path);
			std::vector<std::byte> Bytes;
			std::vector<FAssetReferenceEdge> SourceReferences;
			FAssetPackageFingerprint SourceFingerprint;
			if (!FFileHelper::LoadFileToArray(Bytes, Stored.PhysicalPath))
				return Error(EAssetError::IoError, std::format(
					"Could not read asset package {}.", Stored.PhysicalPath));
			FAssetResult Result = PackageObjectStream::ExtractAssetPackageReferences(
				Bytes, Path, SourceReferences, &SourceFingerprint);
			if (!Result)
			{
				Result.Message = std::format(
					"{} ({})", Result.Message, Stored.PhysicalPath);
				return Result;
			}
			if (SourceReferences.size() > MaximumReferencesPerSnapshot
				|| Publication.ReferenceEdges.size() > MaximumReferencesPerSnapshot
					- SourceReferences.size())
				return Error(EAssetError::CorruptFile,
					"AssetReferenceIndexSnapshotExceeded: mutation exceeds 1,000,000 occurrences.");
			SourceFingerprint.LastWriteTimeTicks = Stored.LastWriteTimeTicks;
			for (FAssetReferenceEdge& Reference : SourceReferences)
				Reference.SourceFingerprint = SourceFingerprint;
			Publication.ReferenceEdges.insert(Publication.ReferenceEdges.end(),
				std::make_move_iterator(SourceReferences.begin()),
				std::make_move_iterator(SourceReferences.end()));
			Publication.ReferenceFingerprints.insert_or_assign(
				Path, SourceFingerprint);
		}
		std::ranges::sort(Publication.ReferenceEdges, &AssetReferenceLess);
		Publication.ReferenceErrors.clear();
		Publication.bReferenceIndexComplete =
			Publication.ReferenceFingerprints.size() == Publication.Assets.size();
		return PublishPreparedState(ExpectedRevision, {
			.Assets = std::move(Publication.Assets),
			.ReferenceEdges = std::move(Publication.ReferenceEdges),
			.ReferenceFingerprints = std::move(Publication.ReferenceFingerprints),
			.ReferenceErrors = std::move(Publication.ReferenceErrors),
			.ReferenceStats = Publication.ReferenceStats,
			.ReferenceCacheWarning = std::move(Publication.ReferenceCacheWarning),
			.bReferenceIndexComplete = Publication.bReferenceIndexComplete});
	}

	auto FAssetPublicationCoordinator::CapturePreparedState() const
		-> FAssetPublicationState
	{
		FAssetRegistryPublication Publication = CaptureAssetRegistryPublication();
		return {
			.Assets = std::move(Publication.Assets),
			.ReferenceEdges = std::move(Publication.ReferenceEdges),
			.ReferenceFingerprints = std::move(Publication.ReferenceFingerprints),
			.ReferenceErrors = std::move(Publication.ReferenceErrors),
			.ReferenceStats = Publication.ReferenceStats,
			.ReferenceCacheWarning = std::move(Publication.ReferenceCacheWarning),
			.bReferenceIndexComplete = Publication.bReferenceIndexComplete};
	}

	auto FAssetPublicationCoordinator::PublishPreparedState(uint64 ExpectedRevision,
		FAssetPublicationState State) -> FAssetResult
	{
		if (GetAssetCatalogRevision() != ExpectedRevision)
			return Error(EAssetError::StaleData, std::format(
				"Asset registry publication expected revision {} but current revision is {}.",
				ExpectedRevision, GetAssetCatalogRevision()));
		return PublishAssetRegistryPublication({
			.ExpectedRevision = ExpectedRevision,
			.Assets = std::move(State.Assets),
			.ReferenceEdges = std::move(State.ReferenceEdges),
			.ReferenceFingerprints = std::move(State.ReferenceFingerprints),
			.ReferenceErrors = std::move(State.ReferenceErrors),
			.ReferenceStats = State.ReferenceStats,
			.ReferenceCacheWarning = std::move(State.ReferenceCacheWarning),
			.bReferenceIndexComplete = State.bReferenceIndexComplete});
	}
}
