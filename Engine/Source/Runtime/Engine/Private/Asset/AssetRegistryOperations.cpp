#include "Asset/RegistryOperations.h"
#include "AssetRegistry/Scan.h"
#include "AssetRegistryOperationsInternal.h"
#include "AssetRegistryResultAdapter.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetMutationRegistryInternal.h"

#include "DObject/Class.h"
#include "Misc/FileTime.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	namespace
	{
		template<typename TResolution>
		auto AssetPathResolutionError(const TResolution& Resolution) -> FAssetResult;

		auto ValidateLoadedAssetClass(std::string_view Name, const DClass* Expected,
			bool bTopLevel) -> EAssetPathResolveState
		{
			const DClass* Class = FindClassByQualifiedName(FName(Name));
			if (!Class) return EAssetPathResolveState::UnknownTargetClass;
			return Expected && bTopLevel && !Class->IsChildOf(Expected)
				? EAssetPathResolveState::RedirectTypeMismatch : EAssetPathResolveState::Resolved;
		}

		auto ValidateAdmission(const FAssetRegistrySnapshot& Snapshot,
			std::span<const FPackagePath> Paths) -> FAssetResult
		{
			const auto Admission = ValidateAssetRegistryParticipants(Snapshot.Catalog, Paths);
			if (Admission) return {};
			return {EAssetError::StaleData,
				std::format("Registry participant admission failed: {}", Admission.FailedParticipant.GetView()),
				{Admission.State == EAssetRegistryAdmissionState::ProjectionPending
					? EAssetResultDisposition::ContentCommittedProjectionPending : EAssetResultDisposition::Default}};
		}
	}

	auto ResolveAssetPathForOperation(const FPackagePath& Path,
		const FAssetPathResolveOptions& Options) -> FAssetPathResolveResult
	{
		auto Result = ResolveAssetPath(Path);
		if (Result && Result.FinalAssetData)
		{
			const auto& Assets = Result.FinalAssetData->TopLevelAssets;
			if (Assets.size() == 1)
				Result.State = ValidateLoadedAssetClass(Assets.front().AssetClassName, Options.ExpectedClass, true);
			else if (Options.ExpectedClass) Result.State = EAssetPathResolveState::RedirectTypeMismatch;
		}
		return Result;
	}

	auto ResolveAssetObjectPathForOperation(const FObjectPath& Path,
		const FAssetPathResolveOptions& Options) -> FObjectPathResolveResult
	{
		auto Result = ResolveAssetObjectPath(Path);
		if (Result && Result.FinalAssetData)
			Result.State = ValidateLoadedAssetClass(Result.FinalAssetData->AssetClassName,
				Options.ExpectedClass, Result.FinalPath.IsTopLevelAsset());
		return Result;
	}

	auto ValidateResolvedAssetForOperation(const FAssetRegistrySnapshot& Snapshot,
		const FAssetPathResolveResult& Resolution, const DClass* ExpectedClass) -> FAssetResult
	{
		if (!Resolution) return AssetPathResolutionError(Resolution);
		std::vector<FPackagePath> Paths = Resolution.RedirectChain;
		Paths.push_back(Resolution.RequestedPath);
		Paths.push_back(Resolution.FinalPath);
		if (auto Admission = ValidateAdmission(Snapshot, Paths); !Admission) return Admission;
		auto Checked = Resolution;
		const auto& Assets = Checked.FinalAssetData->TopLevelAssets;
		if (Assets.size() == 1)
			Checked.State = ValidateLoadedAssetClass(Assets.front().AssetClassName, ExpectedClass, true);
		else if (ExpectedClass) Checked.State = EAssetPathResolveState::RedirectTypeMismatch;
		return Checked ? FAssetResult{} : AssetPathResolutionError(Checked);
	}

	auto ValidateResolvedAssetForOperation(const FAssetRegistrySnapshot& Snapshot,
		const FObjectPathResolveResult& Resolution, const DClass* ExpectedClass) -> FAssetResult
	{
		if (!Resolution) return AssetPathResolutionError(Resolution);
		std::vector<FPackagePath> Paths{Resolution.RequestedPath.GetPackagePath(), Resolution.FinalPath.GetPackagePath()};
		for (const auto& Alias : Resolution.RedirectChain) Paths.push_back(Alias.GetPackagePath());
		if (auto Admission = ValidateAdmission(Snapshot, Paths); !Admission) return Admission;
		auto Checked = Resolution;
		Checked.State = ValidateLoadedAssetClass(Checked.FinalAssetData->AssetClassName,
			ExpectedClass, Checked.FinalPath.IsTopLevelAsset());
		return Checked ? FAssetResult{} : AssetPathResolutionError(Checked);
	}

	using AssetPrivate::FMutationPackageMetadata;
	using AssetPrivate::ValidateMutationPackageMetadata;

	namespace
	{
		auto ResolveAuthoredPackagePath(const FPackagePath& Path) -> std::string
		{
			const FAssetPathResult Resolved = FMountPaths::ResolveAssetPath(
				Path.GetView(), EMountPathExistence::AllowMissing);
			return Resolved
				? Resolved.PhysicalPath.generic_string() + ".dasset"
				: std::string{};
		}

		auto ProjectTopLevelAssets(const FAssetPackageHeader& Header)
			-> std::vector<FTopLevelAssetData>
		{
			std::vector<FTopLevelAssetData> Result;
			Result.reserve(Header.TopLevelAssets.size());
			for (const FAssetPackageTopLevelAssetHeader& Asset : Header.TopLevelAssets)
				Result.push_back({Asset.AssetPath, Asset.AssetClassName,
					Asset.RedirectDestination});
			return Result;
		}

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		template<typename TResolution>
		auto AssetPathResolutionError(
			const TResolution& Resolution
		) -> FAssetResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::ProjectionPending:
				return {EAssetError::StaleData,
					std::format("Registry projection for package {} is pending synchronization.",
						Resolution.FinalPath.ToString()),
					{EAssetResultDisposition::ContentCommittedProjectionPending}};
			case EAssetPathResolveState::NotFound:
				return Error(EAssetError::NotFound, std::format("Asset {} is not present in the registry.", Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::MissingRedirectTarget:
				return Error(EAssetError::NotFound, std::format("Asset redirect {} has a missing target {}.", Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectCycle:
				return Error(EAssetError::CircularDependency, std::format("Asset redirect {} contains a cycle at {}.", Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectDepthExceeded:
				return Error(EAssetError::CircularDependency, std::format("Asset redirect {} exceeds the maximum redirect depth at {}.", Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::UnknownTargetClass:
				return Error(EAssetError::UnknownClass, std::format("Asset {} resolves to a target with an unavailable reflected class.", Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::RedirectTypeMismatch:
				return Error(EAssetError::TypeMismatch, std::format("Asset {} resolves to a target with an incompatible class.", Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::CorruptRedirector:
				return Error(EAssetError::CorruptFile, std::format("CorruptRedirector: asset {} traverses invalid redirect metadata at {}.", Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			}
			return Error(
				EAssetError::CorruptFile,
				"Asset resolution returned an unknown state."
			);
		}
	} // namespace

	auto BuildCookReachability(
		std::span<const FPackagePath> Roots,
		std::vector<FPackagePath>& OutPackages
	) -> FAssetResult
	{
		return BuildCookReachability(
			CaptureAssetRegistrySnapshot(), Roots, OutPackages
		);
	}

	auto BuildCookReachability(
		const FAssetRegistrySnapshot& RegistrySnapshot,
		std::span<const FPackagePath> Roots,
		std::vector<FPackagePath>& OutPackages
	) -> FAssetResult
	{
		OutPackages.clear();
		FAssetReferenceStoreCapture ExternalRoots;
		FAssetResult Result = CaptureAssetReferenceStores(ExternalRoots);
		if (!Result)
		{
			Result.Message = std::format("CookReachabilityExternalRootProviderFailed: {}",
				Result.Message);
			return Result;
		}
		return BuildCookReachability(RegistrySnapshot, ExternalRoots, Roots, OutPackages);
	}

	auto BuildCookReachability(
		const FAssetRegistrySnapshot& RegistrySnapshot,
		const FAssetReferenceStoreCapture& ExternalRoots,
		std::span<const FPackagePath> Roots,
		std::vector<FPackagePath>& OutPackages
	) -> FAssetResult
	{
		OutPackages.clear();
		const FAssetCatalogSnapshot& Catalog = RegistrySnapshot.Catalog;
		const FAssetReferenceIndex& ReferenceIndex = RegistrySnapshot.References;
		struct FPendingCookPath
		{
			FPackagePath Path;
			std::string ExpectedClass;
			std::string Source;
		};
		std::vector<FPendingCookPath> Pending;
		Pending.reserve(Roots.size());
		for (const FPackagePath& Root : Roots)
			Pending.push_back({Root, {}, "explicit Cook root"});
		for (const FAssetReferenceStoreSnapshot& Snapshot : ExternalRoots.Stores)
		{
			for (const FAssetReferenceStoreOccurrence& Occurrence :
				 Snapshot.Occurrences)
				if (Occurrence.bCookRoot)
					Pending.push_back({Occurrence.TargetPath, Occurrence.ExpectedClass, Occurrence.DisplayRoute});
		}
		std::unordered_set<FPackagePath> Visited;
		while (!Pending.empty())
		{
			std::ranges::sort(Pending, [](const FPendingCookPath& Left, const FPendingCookPath& Right) {
				return Left.Path.GetView() > Right.Path.GetView();
			});
			FPendingCookPath Requested = std::move(Pending.back());
			Pending.pop_back();
			DClass* ExpectedClass = nullptr;
			if (!Requested.ExpectedClass.empty())
			{
				ExpectedClass = FindClassByQualifiedName(FName(Requested.ExpectedClass));
				if (!ExpectedClass)
					return Error(EAssetError::UnknownClass, std::format("CookReachabilityUnknownRootClass: {} expects unavailable class {}.", Requested.Source, Requested.ExpectedClass));
			}
			const FAssetPathResolveResult SourceResolution = RegistrySnapshot.ResolveAssetPath(
				Requested.Path
			);
			if (!SourceResolution)
			{
				FAssetResult ResolutionError = AssetPathResolutionError(SourceResolution);
				if (ResolutionError.Error == EAssetError::NotFound)
					ResolutionError.Error = EAssetError::MissingDependency;
				ResolutionError.Message = std::format(
					"CookReachabilityUnresolvedRoot: {} from {}. {}",
					Requested.Path.ToString(), Requested.Source,
					ResolutionError.Message
				);
				return ResolutionError;
			}
			if (auto Validation = ValidateResolvedAssetForOperation(
				RegistrySnapshot, SourceResolution, ExpectedClass); !Validation) return Validation;
			const FPackagePath Source = SourceResolution.FinalPath;
			if (!Visited.insert(Source).second) continue;
			const FAssetData* SourceData = Catalog.FindExact(Source);
			if (!SourceData || SourceData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetError::InvalidPackageType, std::format("CookReachabilityNonAssetPackage: {} is not a real asset.", Source.ToString()));
			if (!ReferenceIndex.GetSourceFingerprints().contains(Source))
				return Error(EAssetError::StaleData, std::format("CookReachabilityIncompleteReferenceIndex: {} has no current source entry.", Source.ToString()));
			for (const FPackagePath& Dependency : SourceData->Dependencies)
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
						Source.ToString(), Dependency.ToString(), ResolutionError.Message
					);
					return ResolutionError;
				}
				if (auto Validation = ValidateResolvedAssetForOperation(
					RegistrySnapshot, Resolution); !Validation) return Validation;
				Pending.push_back({Dependency, {}, std::format("hard dependency of {}", Source.ToString())});
			}
			FAssetPackageInspection Inspection;
			FAssetResult InspectionResult = InspectAssetPackage(
				SourceData->PhysicalPath, Source, Inspection);
			if (!InspectionResult) return InspectionResult;
			std::vector<FAssetReferenceEdge> ExactReferences;
			InspectionResult = ExtractAssetReferences(
				Source, Inspection, ExactReferences);
			if (!InspectionResult) return InspectionResult;
			for (const FAssetReferenceEdge& Reference : ExactReferences)
			{
				if (Reference.Kind == EAssetReferenceKind::Redirect) continue;
				DClass* ReferenceClass = nullptr;
				if (!Reference.ExpectedClass.empty())
				{
					ReferenceClass = FindClassByQualifiedName(
						FName(Reference.ExpectedClass));
					if (!ReferenceClass)
						return Error(EAssetError::UnknownClass, std::format(
							"CookReachabilityUnknownReferenceClass: {} expects unavailable class {}.",
							Reference.DisplayRoute, Reference.ExpectedClass));
				}
				const FObjectPathResolveResult Resolution =
					RegistrySnapshot.ResolveAssetObjectPath(Reference.TargetPath);
				if (!Resolution)
				{
					FAssetResult ResolutionError = AssetPathResolutionError(Resolution);
					if (ResolutionError.Error == EAssetError::NotFound)
						ResolutionError.Error = EAssetError::MissingDependency;
					ResolutionError.Message = std::format(
						"CookReachabilityUnresolvedReference: {} references {}. {}",
						Source.ToString(), Reference.TargetPath.ToString(),
						ResolutionError.Message
					);
					return ResolutionError;
				}
				if (auto Validation = ValidateResolvedAssetForOperation(
					RegistrySnapshot, Resolution, ReferenceClass); !Validation) return Validation;
				Pending.push_back({Resolution.FinalPath.GetPackagePath(), {}, Reference.DisplayRoute});
			}
		}
		OutPackages.assign(Visited.begin(), Visited.end());
		std::ranges::sort(OutPackages, [](const FPackagePath& Left, const FPackagePath& Right) {
			return Left.GetView() < Right.GetView();
		});
		return {};
	}

	auto RefreshSavedPackages(
		std::span<const FPackagePath> Paths) -> FAssetResult
	{
		if (Paths.empty()) return {};
		FAssetRegistryPublication Current = CaptureAssetRegistryPublication();
		FAssetRegistryDelta Delta{.ExpectedRevision = Current.ExpectedRevision};
		std::unordered_set<FPackagePath> Seen;
		for (const FPackagePath& Path : Paths)
		{
			if (!Path.IsValid() || !Seen.insert(Path).second)
				return Error(EAssetError::InvalidPath,
					"Projection reconcile contains an invalid or duplicate path.");
			const std::string PhysicalPath = ResolveAuthoredPackagePath(Path);
			std::error_code Ec;
			if (PhysicalPath.empty() || !std::filesystem::is_regular_file(PhysicalPath, Ec))
			{
				if (Current.Assets.contains(Path)) Delta.Removes.push_back(Path);
				continue;
			}
			FAssetPackageHeader Header;
			if (FAssetRegistryResult HeaderResult = ReadAssetPackageHeader(
				PhysicalPath, Path, Header); !HeaderResult)
				return AssetPrivate::ToAssetResult(std::move(HeaderResult));
			const auto WriteTime = std::filesystem::last_write_time(PhysicalPath, Ec);
			if (Ec) return Error(EAssetError::IoError,
				"Projection reconcile could not read a package timestamp.");
			const uintmax_t FileSize = std::filesystem::file_size(PhysicalPath, Ec);
			if (Ec) return Error(EAssetError::IoError,
				"Projection reconcile could not read a package size.");
			FAssetData Data{
				.PackagePath = Path,
				.PhysicalPath = PhysicalPath,
				.TopLevelAssets = ProjectTopLevelAssets(Header),
				.AssetClassName = Header.AssetClassName,
				.EntryKind = Header.EntryKind,
				.RedirectDestination = Header.RedirectDestination,
				.FormatVersion = Header.FormatVersion,
				.Dependencies = Header.Dependencies,
				.SoftDependencies = Header.SoftDependencies,
				.SearchableNames = Header.SearchableNames,
				.ObjectCount = Header.ObjectCount,
				.BulkSegmentExtent = Header.BulkSegmentExtent,
				.BulkSegmentDigest = Header.BulkSegmentDigest,
				.FileSize = FileSize,
				.LastWriteTime = WriteTime,
				.LastWriteTimeTicks = FileTime::ToStableTicks(WriteTime)};
			if (Current.Assets.contains(Path)) Delta.Replaces.push_back(std::move(Data));
			else Delta.Adds.push_back(std::move(Data));
			Delta.ReferenceInvalidations.push_back(Path);
		}
		return AssetPrivate::ToAssetResult(PublishAssetRegistryDelta(std::move(Delta)));
	}

	auto FlushAssetCatalogSnapshotForTesting() -> void { FlushAssetRegistryCaches(); }
	auto IsAssetCatalogSnapshotDirtyForTesting() -> bool { return IsAssetRegistryCacheDirty(); }
	auto GetAssetCatalogCacheWarningForTesting() -> std::string { return GetAssetRegistryCacheWarning(); }
} // namespace Durin
