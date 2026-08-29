#include "AssetRuntimeStateInternal.h"
#include "AssetRegistry/Scan.h"
#include "AssetDeletionInternal.h"
#include "AssetRegistry/Publication.h"
#include "AssetMutationJournalInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetRelocationExtensionsInternal.h"
#include "Asset/PackageObjectStreamReader.h"
#include "AssetPackageCodec.h"
#include "Asset/PackageVersionPolicy.h"
#include "Asset/Redirector.h"
#include "AssetPackageArchive.h"
#include "AssetPackageValueCodec.h"
#include "Profiling/Profiling.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"

#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"
#include "Threading/RunnableThread.h"

namespace Durin::Asset
{
	using Private::FAssetReferenceStoreRegistry;
	using Private::GetAssetReferenceStoreRegistry;
	using Private::EAssetMutationState;
	using Private::ERelocationPublicationRole;
	using Private::FAssetMutationJournal;
	using Private::FAssetMutationJournalEntry;
	using Private::FingerprintRelocationFile;
	using Private::IsWritableRelocationPath;
	using Private::LoadRelocationBytes;
	using Private::MakePackageFingerprint;
	using Private::MakeRelocationOperationId;
	using Private::NormalizePhysicalPath;
	using Private::PublishRelocationFile;
	using Private::RebuildReferenceProjectionForPublishedEntries;
	using Private::SaveRelocationBytes;
	using Private::WriteMutationJournalState;
	using Private::AssetReferenceLess;

	namespace
	{
		thread_local FAssetLoadReport* GActiveAssetLoadReport = nullptr;
		thread_local uint64 GActivePackageFileReadCount = 0;

		auto CheckSoftObjectThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto Error(EAssetError Code, std::string Message) -> FAssetResult;
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			std::span<const std::byte> Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult;

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
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectCycle:
				return Error(EAssetError::CircularDependency, std::format(
					"Asset redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectDepthExceeded:
				return Error(EAssetError::CircularDependency, std::format(
					"Asset redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
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
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			}
			return Error(EAssetError::CorruptFile, "Asset resolution returned an unknown state.");
		}

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto CorruptRedirector(std::string Message) -> FAssetResult
		{
			return Error(EAssetError::CorruptFile,
				std::format("CorruptRedirector: {}", Message));
		}

		constexpr uint32 MaximumRedirectDepth = 32;
	}

	auto FAssetLoadReport::HasNonUpgradeMutations() const -> bool
	{
		return std::ranges::any_of(Mutations, [](const FAssetLoadMutation& Mutation) {
			return Mutation.Kind == EAssetLoadMutationKind::NonUpgrade;
		});
	}

	auto ReportAssetLoadMutation(
		DObject* Object,
		std::string HandlerId,
		std::string Summary,
		EAssetLoadMutationKind Kind) -> void
	{
		if (!GActiveAssetLoadReport || !Object) return;
		DPackage* Package = Object->GetPackage();
		FAssetPath PackagePath;
		if (Package) FAssetPath::TryCreate(Package->GetPackagePath(), PackagePath);
		GActiveAssetLoadReport->Mutations.push_back({
			.PackagePath = std::move(PackagePath),
			.ObjectPath = Object->GetObjectPath(),
			.HandlerId = std::move(HandlerId),
			.Summary = std::move(Summary),
			.Kind = Kind});
	}




	auto FAssetRuntimeState::Get() -> FAssetRuntimeState&
	{
		static FAssetRuntimeState Instance;
		return Instance;
	}

	FAssetRuntimeState::FAssetRuntimeState()
		: Loader(GetAssetPublicationCoordinator(), Residency, RuntimeConfiguration, bAcceptingRequests)
		, Mutations(
			GetAssetPublicationCoordinator(),
			Residency,
			Loader,
			RuntimeConfiguration,
			bAcceptingRequests)
	{
	}

	auto FAssetLoadService::CreateAsset(const FAssetPath& Path, DClass* Class, size_t Size, DObject*& OutAsset) -> FAssetResult
	{
		OutAsset = nullptr;
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown, "Asset creation is closed while the asset manager is shutting down.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit asset creation.");
		if (!Path.IsValid() || !Class || !Class->ClassConstructor) return Error(EAssetError::InvalidPath, "Invalid asset path or class.");
		if (const FAssetCatalogEntry Existing = FindAssetExact(Path))
			return Existing->EntryKind == EAssetRegistryEntryKind::Redirector
				? Error(EAssetError::AlreadyExists, std::format(
					"Asset {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another destination.",
					Path.ToString(), Existing->RedirectDestination.ToString()))
				: Error(EAssetError::AlreadyExists, std::format(
					"Asset {} already exists. Choose another destination or delete the existing asset.",
					Path.ToString()));
		if (ResidentPackages.contains(Path))
			return Error(EAssetError::AlreadyExists, std::format(
				"A loaded package already uses {}. Close it or choose another destination.",
				Path.ToString()));

		DPackage* Package = NewObject<DPackage>(nullptr, FName(Path.GetAssetName()));
		Package->InitializeAssetPackage(Path);
		AddToRoot(Package);
		FStaticConstructObjectParameters Params{
			Class, Package, FName(Path.GetAssetName()), Size,
			EObjectFlags::Public};
		OutAsset = StaticConstructObject(Params);
		DObjectForceRegistration(OutAsset);
		if (!Package->SetAsset(OutAsset))
		{
			if (Package->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
				RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
			CollectGarbage();
			OutAsset = nullptr;
			return Error(EAssetError::InvalidObjectGraph, "Failed to assign package asset.");
		}
		ResidentPackages.emplace(
			Path, Package, EAssetPackagePublicationState::NewlyCreated);
		return {};
	}

	auto FAssetLoadService::DuplicateAsset(
		const FAssetPath& SourcePath,
		const FAssetPath& DestinationPath,
		DObject*& OutAsset) -> FAssetResult
	{
		OutAsset = nullptr;
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown,
				"Asset duplication is closed while the asset manager is shutting down.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode,
				"Cooked runtime package mode does not permit asset duplication.");
		if (!SourcePath.IsValid() || !DestinationPath.IsValid()
			|| SourcePath == DestinationPath)
			return Error(EAssetError::InvalidPath,
				"Asset duplication requires distinct valid source and destination paths.");
		const FAssetCatalogEntry SourceData = FindAssetExact(SourcePath);
		if (!SourceData)
			return Error(EAssetError::NotFound,
				std::format("Asset {} is not registered.", SourcePath.ToString()));
		if (SourceData->EntryKind != EAssetRegistryEntryKind::Asset)
			return Error(EAssetError::InvalidPackageType,
				"Redirectors cannot be duplicated as assets. Duplicate the final asset instead.");
		if (const FAssetCatalogEntry Existing = FindAssetExact(DestinationPath))
			return Existing->EntryKind == EAssetRegistryEntryKind::Redirector
				? Error(EAssetError::AlreadyExists, std::format(
					"Asset {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another destination.",
					DestinationPath.ToString(), Existing->RedirectDestination.ToString()))
				: Error(EAssetError::AlreadyExists, std::format(
					"Asset {} already exists. Choose another destination or delete the existing asset.",
					DestinationPath.ToString()));
		if (ResidentPackages.contains(DestinationPath))
			return Error(EAssetError::AlreadyExists, std::format(
				"A loaded package already uses {}. Close it or choose another destination.",
				DestinationPath.ToString()));

		DObject* SourceAsset = nullptr;
		FAssetResult Result = LoadAsset(SourcePath, SourceAsset);
		if (!Result) return Result;
		if (!SourceAsset)
			return Error(EAssetError::InvalidObjectGraph,
				"The source package has no main asset to duplicate.");

		DPackage* Package = NewObject<DPackage>(
			nullptr, FName(DestinationPath.GetAssetName()));
		Package->InitializeAssetPackage(DestinationPath);
		AddToRoot(Package);
		OutAsset = DuplicateObject(
			SourceAsset,
			Package,
			FName(DestinationPath.GetAssetName()));
		if (!OutAsset || !Package->SetAsset(OutAsset))
		{
			if (Package->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
				RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
			CollectGarbage();
			OutAsset = nullptr;
			return Error(EAssetError::InvalidObjectGraph,
				"Failed to assign the duplicated package asset.");
		}
		ResidentPackages.emplace(
			DestinationPath,
			Package,
			EAssetPackagePublicationState::NewlyCreated);
		return {};
	}

	auto FAssetLoadService::CreateRedirector(
		const FAssetPath& RedirectorPath,
		const FAssetPath& DestinationPath,
		DAssetRedirector*& OutRedirector) -> FAssetResult
	{
		OutRedirector = nullptr;
		if (!RedirectorPath.IsValid() || !DestinationPath.IsValid()
			|| RedirectorPath == DestinationPath)
			return Error(EAssetError::InvalidPath,
				"Redirector source and destination paths must be valid and distinct.");
		const FAssetPathResolveResult Resolution = Durin::Asset::ResolveAssetPath(DestinationPath);
		if (!Resolution)
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::NotFound:
			case EAssetPathResolveState::MissingRedirectTarget:
				return Error(EAssetError::NotFound,
					"Redirector destination does not resolve to a registered asset.");
			case EAssetPathResolveState::RedirectCycle:
			case EAssetPathResolveState::RedirectDepthExceeded:
				return Error(EAssetError::CircularDependency,
					"Redirector destination does not have a finite canonical target.");
			case EAssetPathResolveState::UnknownTargetClass:
				return Error(EAssetError::UnknownClass,
					"Redirector destination has an unavailable reflected class.");
			case EAssetPathResolveState::RedirectTypeMismatch:
				return Error(EAssetError::TypeMismatch,
					"Redirector destination has an incompatible asset class.");
			case EAssetPathResolveState::CorruptRedirector:
				return CorruptRedirector(
					"the requested destination traverses corrupt redirect metadata.");
			case EAssetPathResolveState::Resolved:
				break;
			}
		}
		DObject* DestinationObject = nullptr;
		FAssetResult Result = LoadAsset(Resolution.FinalPath, DestinationObject);
		if (!Result) return Result;
		DObject* CreatedObject = nullptr;
		Result = CreateAsset(
			RedirectorPath,
			DAssetRedirector::StaticClass(),
			sizeof(DAssetRedirector),
			CreatedObject);
		if (!Result) return Result;
		OutRedirector = Cast<DAssetRedirector>(CreatedObject);
		if (!OutRedirector)
			return Error(EAssetError::InvalidObjectGraph,
				"Failed to construct the redirector main asset.");
		OutRedirector->SetDestinationObject(DestinationObject);
		return {};
	}

	auto FAssetLoadService::LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return LoadAsset(Path, nullptr, OutAsset, OutReport);
	}

	auto FAssetLoadService::LoadAsset(
		const FAssetPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		OutAsset = nullptr;
		if (OutReport) *OutReport = {
			.RequestedPath = Path,
			.PackagePath = Path};
		auto Finish = [&](FAssetResult Result) -> FAssetResult
		{
			if (OutReport)
			{
				OutReport->Error = Result.Error;
				OutReport->ErrorMessage = Result.Message;
			}
			return Result;
		};
		if (!bAcceptingRequests)
			return Finish(Error(
				EAssetError::ShuttingDown,
				"Asset loading is closed while the asset manager is shutting down."));
		if (ExpectedClass && !ExpectedClass->IsChildOf(DObject::StaticClass()))
			return Finish(Error(EAssetError::TypeMismatch, "An asset load requires a DObject class."));
		if (DPackage* Resident = FindResidentPackage(Path))
		{
			DObject* Asset = Resident->GetAsset();
			if (Asset && !Asset->IsA<DAssetRedirector>())
			{
				if (ExpectedClass && !Asset->IsA(ExpectedClass))
					return Finish(Error(EAssetError::TypeMismatch, std::format(
						"Asset {} is not a {}.", Path.ToString(),
						ExpectedClass->GetQualifiedName().ToString())));
				OutAsset = Asset;
				if (OutReport)
				{
					OutReport->CatalogRevision = GetAssetCatalogRevision();
					OutReport->FinalPath = Path;
					OutReport->FinalAssetClassName =
						Asset->GetClass()->GetQualifiedName().ToString();
				}
				return Finish({});
			}
		}

		const FAssetPathResolveResult Resolution = Durin::Asset::ResolveAssetPath(
			Path, {.ExpectedClass = ExpectedClass});
		if (OutReport)
		{
			OutReport->CatalogRevision = Resolution.CatalogRevision;
			OutReport->FinalPath = Resolution.FinalPath;
			OutReport->RedirectChain = Resolution.RedirectChain;
			if (Resolution.FinalAssetData)
				OutReport->FinalAssetClassName = Resolution.FinalAssetData->AssetClassName;
		}
		if (!Resolution)
			return Finish(AssetPathResolutionError(Resolution));
		check(Resolution.FinalAssetData.has_value());
		return Finish(LoadAssetFromCatalog(
			*Resolution.FinalAssetData, ExpectedClass, OutAsset, OutReport));
	}

	auto FAssetLoadService::LoadAssetFromCatalog(
		const FAssetData& Data,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return LoadAssetFromPhysicalPath(
			Data.PackagePath, Data.PhysicalPath, ExpectedClass, OutAsset, OutReport);
	}

	auto FAssetLoadService::LoadAssetFromPhysicalPath(
		const FAssetPath& Path,
		std::string_view PhysicalPath,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.Load");
		if (!bAcceptingRequests)
		{
			OutAsset = nullptr;
			return Error(
				EAssetError::ShuttingDown,
				"Asset loading is closed while the asset manager is shutting down.");
		}
		if (OutReport)
		{
			if (!OutReport->RequestedPath.IsValid())
				*OutReport = {
					.RequestedPath = Path,
					.FinalPath = Path,
					.PackagePath = Path};
			else OutReport->PackagePath = Path;
		}
		const bool bRootLoad = LoadDepth++ == 0;
		if (bRootLoad) GActivePackageFileReadCount = 0;
		FAssetLoadReport FailureReport;
		if (bRootLoad)
		{
			if (OutReport) FailureReport = *OutReport;
			TransactionPackages.clear();
		}
		FAssetLoadReport* PreviousLoadReport = GActiveAssetLoadReport;
		if (bRootLoad) GActiveAssetLoadReport = OutReport;
		DPackage* Package = nullptr;
		FAssetResult Result = LoadPackageInternal(
			Path, PhysicalPath, Package, OutReport);
		if (Result && Package && ExpectedClass)
		{
			DObject* Asset = Package->GetAsset();
			if (!Asset || !Asset->IsA(ExpectedClass))
				Result = Error(EAssetError::TypeMismatch, std::format(
					"Asset {} is not a {}.", Path.ToString(),
					ExpectedClass->GetQualifiedName().ToString()));
		}
		if (bRootLoad) GActiveAssetLoadReport = PreviousLoadReport;
		--LoadDepth;
		if (bRootLoad)
		{
			if (!Result)
			{
				bool bDiscardedPackage = false;
				for (auto It = TransactionPackages.rbegin(); It != TransactionPackages.rend(); ++It)
				{
					auto LoadedIt = ResidentPackages.find(*It);
					if (LoadedIt == ResidentPackages.end()) continue;
					DPackage* TransactionPackage = LoadedIt->second;
					ResidentPackages.erase(LoadedIt);
					LoadingPackages.erase(*It);
					if (TransactionPackage->HasAnyInternalFlags(
						EObjectInternalFlags::RootSet))
						RemoveFromRoot(TransactionPackage);
					MarkObjectHierarchyAsGarbage(TransactionPackage);
					bDiscardedPackage = true;
				}
				if (bDiscardedPackage) CollectGarbage();
				if (OutReport) *OutReport = std::move(FailureReport);
			}
			TransactionPackages.clear();
		}
		OutAsset = Result && Package ? Package->GetAsset() : nullptr;
		return Result;
	}

	auto FAssetLoadService::LoadPackageInternal(
		const FAssetPath& Path,
		std::string_view PhysicalPath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.LoadPackage");
		if (auto It = ResidentPackages.find(Path); It != ResidentPackages.end())
		{
			OutPackage = It->second;
			return {};
		}
		FAssetLoadReport LocalReport{.PackagePath = Path};
		FAssetLoadReport* CodecReport = OutReport ? OutReport : &LocalReport;
		std::vector<std::byte> Bytes;
		if (PhysicalPath.empty()) return Error(EAssetError::InvalidPath, "Asset path cannot be resolved in the selected package mode.");
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath)) return Error(EAssetError::NotFound, std::format("Asset {} was not found.", Path.ToString()));
		++GActivePackageFileReadCount;
			const Private::FAssetPackageCodec* Codec = nullptr;
			if (FAssetResult Result = Private::ResolveAssetPackageReader(
				Bytes, Codec); !Result)
			return Result;
		{
			FAssetPackageHeader Header;
			FAssetResult Result = Codec->ReadHeader(
				Bytes, Bytes.size(), Header);
			if (!Result) return Result;
			const Private::FMutationPackageMetadata HeaderMetadata{
				.FormatVersion = Header.FormatVersion,
				.AssetClassName = Header.AssetClassName,
				.EntryKind = Header.EntryKind,
				.RedirectDestination = Header.RedirectDestination,
				.Dependencies = Header.Dependencies};
			Result = Private::ValidateMutationPackageMetadata(
				HeaderMetadata, Header.ObjectCount, &Path);
			if (!Result) return Result;

			DPackage* Package = nullptr;
			Result = Codec->Load(
				Bytes, Path, Package, CodecReport,
				[&](DPackage* LoadedPackage) -> FAssetResult {
					if (!LoadedPackage || ResidentPackages.contains(Path))
						return Error(EAssetError::AlreadyExists,
							"The package skeleton is already resident.");
					ResidentPackages.emplace(Path, LoadedPackage);
					LoadingPackages.insert(Path);
					if (LoadDepth > 0) TransactionPackages.push_back(Path);
					return {};
				},
				[&](DPackage* LoadedPackage) {
					LoadingPackages.erase(Path);
					ResidentPackages.erase(Path);
				});
			CodecReport->PackageFileReadCount = GActivePackageFileReadCount;
			if (!Result) return Result;
			LoadingPackages.erase(Path);
			OutPackage = Package;
			return {};
		}
	}

	auto FAssetLoadService::FindResidentPackage(const FAssetPath& Path) const -> DPackage*
	{
		auto It = ResidentPackages.find(Path);
		return It == ResidentPackages.end() ? nullptr : It->second.Package;
	}

	auto FAssetLoadService::AdoptCreatedPackage(DPackage* Package) -> FAssetResult
	{
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown,
				"Asset package adoption is closed while the asset manager is shutting down.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode,
				"Cooked runtime package mode does not permit package adoption.");
		FAssetPath Path;
		if (!Package || !Package->IsAssetPackage()
			|| !FAssetPath::TryCreate(Package->GetPackagePath(), Path)
			|| FindPackage(Path.GetView()) != Package)
			return Error(EAssetError::InvalidPackageType,
				"Only a live registered asset package can be adopted.");
		if (FindAssetExact(Path))
			return Error(EAssetError::AlreadyExists,
				"A catalog entry already occupies the package path.");
		if (auto Existing = ResidentPackages.find(Path);
			Existing != ResidentPackages.end())
			return Existing->second.Package == Package
				? FAssetResult{}
				: Error(EAssetError::AlreadyExists,
					"A different package is already resident at this path.");
		ResidentPackages.emplace(
			Path, Package, EAssetPackagePublicationState::NewlyCreated);
		return {};
	}

	auto FAssetLoadService::GetResidentPackagePublicationState(
		const FAssetPath& Path) const
		-> std::optional<EAssetPackagePublicationState>
	{
		const auto It = ResidentPackages.find(Path);
		return It == ResidentPackages.end()
			? std::nullopt
			: std::optional{It->second.PublicationState};
	}

	auto FAssetLoadService::IsPackageReferenced(const DPackage* Package) const -> bool
	{
		if (!Package) return false;
		FAssetPath Path;
		if (!FAssetPath::TryCreate(Package->GetPackagePath(), Path)) return false;
		for (const auto& [OtherPath, OtherPackage] : ResidentPackages)
		{
			if (OtherPackage == Package) continue;
			const FAssetCatalogEntry Data = FindAssetExact(OtherPath);
			if (!Data) continue;
			for (const FAssetPath& Dependency : Data->Dependencies)
			{
				const FAssetPathResolveResult Resolution = Durin::Asset::ResolveAssetPath(Dependency);
				if (Resolution && Resolution.FinalPath == Path) return true;
			}
		}
		return false;
	}

	auto FAssetLoadService::UnloadPackage(
		const FAssetPath& Path,
		EAssetPackageUnloadPolicy Policy) -> FAssetResult
	{
		auto It = ResidentPackages.find(Path);
		if (It == ResidentPackages.end())
			return Error(EAssetError::NotFound, "Package is not resident.");
		if (LoadingPackages.contains(Path) || IsPackageReferenced(It->second)) return Error(EAssetError::InUse, "Package is still referenced.");
		DPackage* Package = It->second.Package;
		const bool bHasUnsavedState =
			It->second.PublicationState
				== EAssetPackagePublicationState::NewlyCreated
			|| (Package && Package->IsDirty());
		if (bHasUnsavedState
			&& Policy == EAssetPackageUnloadPolicy::RejectUnsaved)
			return Error(EAssetError::InUse,
				"Package has unsaved state; explicit discard policy is required.");
		ResidentPackages.erase(It);
		if (Package->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
			RemoveFromRoot(Package);
		MarkObjectHierarchyAsGarbage(Package);
		CollectGarbage();
		return {};
	}

	auto FAssetLoadService::CapturePackageLoadSnapshot() const -> FAssetPackageLoadSnapshot
	{
		FAssetPackageLoadSnapshot Snapshot;
		Snapshot.ResidentPackages.reserve(ResidentPackages.size());
		for (const auto& [Path, Package] : ResidentPackages) Snapshot.ResidentPackages.push_back(Path);
		std::ranges::sort(Snapshot.ResidentPackages, {}, [](const FAssetPath& Path) {
			return Path.ToString();
		});
		return Snapshot;
	}

	auto FAssetLoadService::ReleasePackagesLoadedSince(
		const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult
	{
		if (LoadDepth != 0 || !LoadingPackages.empty())
			return Error(EAssetError::InUse, "A package load is still in progress.");

		std::unordered_set<FAssetPath> Protected(
			Snapshot.ResidentPackages.begin(), Snapshot.ResidentPackages.end());
		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;
			for (const auto& [Path, Package] : ResidentPackages)
			{
				if (!Protected.contains(Path)) continue;
				const FAssetCatalogEntry Data = FindAssetExact(Path);
				if (!Data) continue;
				for (const FAssetPath& Dependency : Data->Dependencies)
				{
					const FAssetPathResolveResult Resolution = Durin::Asset::ResolveAssetPath(Dependency);
					if (Resolution) bChanged |= Protected.insert(Resolution.FinalPath).second;
				}
			}
		}

		std::vector<DPackage*> ReleasedPackages;
		for (auto It = ResidentPackages.begin(); It != ResidentPackages.end();)
		{
			if (Protected.contains(It->first)
				|| It->second.PublicationState
					== EAssetPackagePublicationState::NewlyCreated
				|| (It->second.Package && It->second.Package->IsDirty()))
			{
				++It;
				continue;
			}
			DPackage* Package = It->second;
			ReleasedPackages.push_back(Package);
			It = ResidentPackages.erase(It);
		}
		for (DPackage* Package : ReleasedPackages)
		{
			if (Package->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
				RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
		}
		if (!ReleasedPackages.empty()) CollectGarbage();
		return {};
	}

	auto FAssetRuntimeState::Shutdown() -> void
	{
		StopAcceptingRequests();
		FlushAssetRegistryCaches();
		std::vector<DPackage*> Packages;
		Packages.reserve(Residency.size());
		for (const auto& [Path, Package] : Residency)
		{
			if (Package) Packages.push_back(Package);
		}
		Residency.clear();
		Loader.Reset();
		for (DPackage* Package : Packages)
		{
			if (Package->HasAnyInternalFlags(EObjectInternalFlags::RootSet))
				RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
		}
	}

	auto FAssetRuntimeState::Initialize(FAssetRuntimeConfiguration Configuration)
		-> FAssetResult
	{
		if (bAcceptingRequests)
		{
			if (RuntimeConfiguration == Configuration) return {};
			return Error(EAssetError::InUse,
				"Asset runtime configuration cannot be replaced while Engine Asset is initialized.");
		}
		check(Residency.empty());
		check(Loader.IsIdle());
		RuntimeConfiguration = std::move(Configuration);
		bAcceptingRequests = true;
		return {};
	}

	auto FAssetRuntimeState::StopAcceptingRequests() -> void
	{
		if (!bAcceptingRequests) return;
		bAcceptingRequests = false;
		DURIN_DEBUG("Asset manager stopped accepting new requests.");
	}

	auto FAssetLoadService::ResolveSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult
	{
		CheckSoftObjectThread();
		if (!ExpectedClass || !ExpectedClass->IsChildOf(DObject::StaticClass()))
		{
			return {
				.Result = Error(EAssetError::TypeMismatch, "A soft-object resolve requires a DObject class."),
				.State = Reference.IsNull() ? ESoftObjectResolveState::Null : ESoftObjectResolveState::NotLoaded};
		}
		if (Reference.IsNull())
		{
			return NullPolicy == ESoftObjectNullPolicy::Allow
				? FSoftObjectResolveResult{.State = ESoftObjectResolveState::Null}
				: FSoftObjectResolveResult{
					.Result = Error(EAssetError::InvalidPath, "A null soft-object reference is not allowed."),
					.State = ESoftObjectResolveState::Null};
		}

		const FAssetPath& Path = Reference.GetSoftObjectPath().GetAssetPath();
		const FAssetPathResolveResult Resolution = Durin::Asset::ResolveAssetPath(
			Path, {.ExpectedClass = ExpectedClass});
		if (!Resolution)
		{
			if (Resolution.State == EAssetPathResolveState::NotFound)
			{
				DPackage* LoadedPackage = FindResidentPackage(Path);
				DObject* LoadedObject = LoadedPackage ? LoadedPackage->GetAsset() : nullptr;
				if (LoadedObject && !LoadedObject->IsA<DAssetRedirector>())
				{
					if (!LoadedObject->IsA(ExpectedClass))
						return {
							.Result = Error(EAssetError::TypeMismatch, std::format(
								"Asset {} is not a {}.", Path.ToString(),
								ExpectedClass->GetQualifiedName().ToString())),
							.State = ESoftObjectResolveState::NotLoaded};
					std::string ValidationError;
					if (!Reference.TrySetResolvedObject(
						LoadedObject, Path, Path, ExpectedClass, &ValidationError))
						return {
							.Result = Error(EAssetError::InvalidObjectGraph, std::move(ValidationError)),
							.State = ESoftObjectResolveState::NotLoaded};
					return {
						.State = ESoftObjectResolveState::Loaded,
						.Object = LoadedObject,
						.ResolvedPath = Path};
				}
			}
			Reference.ResetResolvedObject();
			return {
				.Result = AssetPathResolutionError(Resolution),
				.State = ESoftObjectResolveState::NotLoaded};
		}

		DPackage* Package = FindResidentPackage(Resolution.FinalPath);
		if (!Package)
		{
			Reference.ResetResolvedObject();
			return {
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}

		DObject* Object = Package->GetAsset();
		if (!Object)
		{
			return {
				.Result = Error(EAssetError::InvalidObjectGraph, std::format(
					"Loaded package {} has no main asset.", Resolution.FinalPath.ToString())),
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}
		if (!Object->IsA(ExpectedClass))
		{
			return {
				.Result = Error(EAssetError::TypeMismatch, std::format(
					"Asset {} is not a {}.",
					Resolution.FinalPath.ToString(), ExpectedClass->GetQualifiedName().ToString())),
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}

		std::string ValidationError;
		if (!Reference.TrySetResolvedObject(
			Object, Path, Resolution.FinalPath, ExpectedClass, &ValidationError))
		{
			return {
				.Result = Error(EAssetError::InvalidObjectGraph, std::move(ValidationError)),
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}
		return {
			.State = ESoftObjectResolveState::Loaded,
			.Object = Object,
			.ResolvedPath = Resolution.FinalPath,
			.bRedirected = !Resolution.RedirectChain.empty()};
	}

	auto FAssetLoadService::LoadSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		ESoftObjectNullPolicy NullPolicy,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		CheckSoftObjectThread();
		OutObject = nullptr;
		FSoftObjectResolveResult Resolved = ResolveSoftObject(
			Reference, ExpectedClass, NullPolicy);
		if (!Resolved) return Resolved.Result;
		if (Resolved.State == ESoftObjectResolveState::Null) return {};
		if (Resolved.State == ESoftObjectResolveState::Loaded)
		{
			OutObject = Resolved.Object;
			return {};
		}

		DObject* LoadedObject = nullptr;
		const FAssetPath& Path = Reference.GetSoftObjectPath().GetAssetPath();
		FAssetResult Result = LoadAsset(
			Path, ExpectedClass, LoadedObject, OutReport);
		if (!Result) return Result;
		if (!LoadedObject)
			return Error(EAssetError::InvalidObjectGraph, std::format(
				"Loaded package {} has no main asset.", Path.ToString()));
		if (!LoadedObject->IsA(ExpectedClass))
		{
			return Error(EAssetError::TypeMismatch, std::format(
				"Asset {} is not a {}.",
				Path.ToString(), ExpectedClass->GetQualifiedName().ToString()));
		}

		std::string ValidationError;
		if (!Reference.TrySetResolvedObject(
			LoadedObject, Path, Resolved.ResolvedPath, ExpectedClass, &ValidationError))
			return Error(EAssetError::InvalidObjectGraph, std::move(ValidationError));
		OutObject = LoadedObject;
		return {};
	}

}
