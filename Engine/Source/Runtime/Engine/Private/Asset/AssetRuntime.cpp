#include "AssetRuntimeStateInternal.h"
#include "AssetRegistry/Scan.h"
#include "AssetDeletionInternal.h"
#include "AssetRegistry/Publication.h"
#include "AssetMutationJournalInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetRelocationExtensionsInternal.h"
#include "Asset/PackageResource.h"
#include "Asset/EditorBulkDataStorage.h"
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

#include "Misc/Paths.h"
#include "Threading/RunnableThread.h"

namespace Durin::Asset
{
	using Private::FAssetReferenceStoreRegistry;
	using Private::GetAssetReferenceStoreRegistry;
	using Private::EAssetMutationState;
	using Private::FAssetMutationJournal;
	using Private::FAssetMutationJournalEntry;
	using Private::FingerprintRelocationFile;
	using Private::LoadRelocationBytes;
	using Private::MakePackageFingerprint;
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

		auto FindPackageObject(DPackage* Package, const FObjectPath& Path) -> DObject*
		{
			if (!Package || Package->GetPackagePathIdentity() != Path.GetPackagePath())
				return nullptr;
			DObject* Current = Package->FindTopLevelAsset(
				FName(Path.GetAssetPath().GetAssetName()));
			for (const std::string_view Name : Path.GetSubobjectNames())
			{
				if (!Current) return nullptr;
				const auto Children = GDObjectArray.GetObjectsWithOuter(
					Current, EObjectQueryScope::LiveOnly);
				const auto Child = std::ranges::find(Children, FName(Name), &DObject::GetFName);
				Current = Child == Children.end() ? nullptr : *Child;
			}
			return Current;
		}

		auto Error(EAssetError Code, std::string Message) -> FAssetResult;
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			std::span<const std::byte> Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult;

		auto ObjectPathResolutionError(
			const FObjectPathResolveResult& Resolution) -> FAssetResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::NotFound:
				return Error(EAssetError::NotFound, std::format(
					"Object {} is not present in the registry.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::MissingRedirectTarget:
				return Error(EAssetError::NotFound, std::format(
					"Object redirect {} has a missing target {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectCycle:
				return Error(EAssetError::CircularDependency, std::format(
					"Object redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectDepthExceeded:
				return Error(EAssetError::CircularDependency, std::format(
					"Object redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::UnknownTargetClass:
				return Error(EAssetError::UnknownClass, std::format(
					"Object {} resolves to a target with an unavailable reflected class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::RedirectTypeMismatch:
				return Error(EAssetError::TypeMismatch, std::format(
					"Object {} resolves to a target with an incompatible class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::CorruptRedirector:
				return Error(EAssetError::CorruptFile, std::format(
					"CorruptRedirector: object {} traverses invalid redirect metadata at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			}
			return Error(EAssetError::CorruptFile,
				"Object resolution returned an unknown state.");
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

		auto GetResidentAssetPackages() -> std::vector<DPackage*>
		{
			std::vector<DPackage*> Packages;
			for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
			{
				DPackage* Package = Cast<DPackage>(Object);
				if (!Package || Package->IsGarbage() || !Package->IsAssetPackage())
					continue;
				if (FindPackage(Package->GetPackagePath()) == Package)
					Packages.push_back(Package);
			}
			return Packages;
		}
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
		FPackagePath PackagePath;
		if (Package) FPackagePath::TryCreate(Package->GetPackagePath(), PackagePath);
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
		: Loader(GetAssetPublicationCoordinator(), RuntimeConfiguration, bAcceptingRequests)
		, Mutations(
			GetAssetPublicationCoordinator(),
			Loader,
			RuntimeConfiguration,
			bAcceptingRequests)
	{
	}

	auto FAssetLoadService::CreateAsset(const FTopLevelAssetPath& Path, DClass* Class, size_t Size, DObject*& OutAsset) -> FAssetResult
	{
		OutAsset = nullptr;
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown, "Asset creation is closed while the asset manager is shutting down.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit asset creation.");
		if (!Path.IsValid() || !Class || !Class->ClassConstructor) return Error(EAssetError::InvalidPath, "Invalid asset path or class.");
		if (const FTopLevelAssetCatalogEntry Existing = FindTopLevelAssetExact(Path))
			return Existing->IsRedirector()
				? Error(EAssetError::AlreadyExists, std::format(
					"Asset {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another destination.",
					Path.ToString(), Existing->RedirectDestination.ToString()))
				: Error(EAssetError::AlreadyExists, std::format(
					"Asset {} already exists. Choose another destination or delete the existing asset.",
					Path.ToString()));
		if (FindResidentPackage(Path.GetPackagePath()))
			return Error(EAssetError::AlreadyExists, std::format(
				"A loaded package already uses {}. Close it or choose another destination.",
				Path.ToString()));

		DPackage* Package = CreatePackage(Path.GetPackagePath());
		if (!Package)
			return Error(EAssetError::AlreadyExists,
				"A live package already occupies the asset path.");
		FStaticConstructObjectParameters Params{
			Class, Package, FName(Path.GetAssetName()), Size,
			EObjectFlags::Public};
		OutAsset = StaticConstructObject(Params);
		DObjectForceRegistration(OutAsset);
		if (!OutAsset || Package->FindTopLevelAsset(OutAsset->GetFName()) != OutAsset)
		{
			MarkObjectHierarchyAsGarbage(Package);
			CollectGarbage();
			OutAsset = nullptr;
			return Error(EAssetError::InvalidObjectGraph, "Failed to register the package asset.");
		}
		Package->MarkDirty();
		Package->MarkAsNewlyCreated();
		return {};
	}

	auto FAssetLoadService::DuplicateAsset(
		const FTopLevelAssetPath& SourcePath,
		const FTopLevelAssetPath& DestinationPath,
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
		const FTopLevelAssetCatalogEntry SourceData =
			FindTopLevelAssetExact(SourcePath);
		if (!SourceData)
			return Error(EAssetError::NotFound,
				std::format("Asset {} is not registered.", SourcePath.ToString()));
		if (SourceData->IsRedirector())
			return Error(EAssetError::InvalidPackageType,
				"Redirectors cannot be duplicated as assets. Duplicate the final asset instead.");
		if (const FTopLevelAssetCatalogEntry Existing =
			FindTopLevelAssetExact(DestinationPath))
			return Existing->IsRedirector()
				? Error(EAssetError::AlreadyExists, std::format(
					"Asset {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another destination.",
					DestinationPath.ToString(), Existing->RedirectDestination.ToString()))
				: Error(EAssetError::AlreadyExists, std::format(
					"Asset {} already exists. Choose another destination or delete the existing asset.",
					DestinationPath.ToString()));
		if (FindResidentPackage(DestinationPath.GetPackagePath()))
			return Error(EAssetError::AlreadyExists, std::format(
				"A loaded package already uses {}. Close it or choose another destination.",
				DestinationPath.ToString()));

		DObject* SourceAsset = nullptr;
		FObjectPath SourceObjectPath;
		if (!FObjectPath::TryCreate(
			SourcePath, std::span<const std::string>{}, SourceObjectPath))
			return Error(EAssetError::InvalidPath,
				"The source top-level asset path is invalid.");
		FAssetResult Result = LoadObject(
			SourceObjectPath, nullptr, SourceAsset);
		if (!Result) return Result;
		if (!SourceAsset)
			return Error(EAssetError::InvalidObjectGraph,
				"The exact source top-level asset is unavailable for duplication.");

		DPackage* Package = CreatePackage(DestinationPath.GetPackagePath());
		if (!Package)
			return Error(EAssetError::AlreadyExists,
				"A live package already occupies the duplication destination.");
		OutAsset = DuplicateObject(
			SourceAsset,
			Package,
			FName(DestinationPath.GetAssetName()));
		if (!OutAsset || Package->FindTopLevelAsset(OutAsset->GetFName()) != OutAsset)
		{
			MarkObjectHierarchyAsGarbage(Package);
			CollectGarbage();
			OutAsset = nullptr;
			return Error(EAssetError::InvalidObjectGraph,
				"Failed to register the duplicated package asset.");
		}
		Package->MarkDirty();
		Package->MarkAsNewlyCreated();
		return {};
	}

	auto FAssetLoadService::CreateRedirector(
		const FPackagePath& RedirectorPath,
		const FPackagePath& DestinationPath,
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
		check(Resolution.FinalAssetData.has_value());
		const FAssetData& DestinationData = *Resolution.FinalAssetData;
		const auto DestinationRecord = std::ranges::find(
			DestinationData.TopLevelAssets, DestinationData.AssetClassName,
			&FTopLevelAssetData::AssetClassName);
		if (DestinationRecord == DestinationData.TopLevelAssets.end())
			return Error(EAssetError::InvalidObjectGraph,
				"The redirect destination has no exact top-level asset.");
		FObjectPath DestinationObjectPath;
		if (!FObjectPath::TryCreate(
			DestinationRecord->AssetPath, std::span<const std::string>{},
			DestinationObjectPath))
			return Error(EAssetError::InvalidPath,
				"The redirect destination object path is invalid.");
		FAssetResult Result = LoadObject(
			DestinationObjectPath, nullptr, DestinationObject);
		if (!Result) return Result;
		DObject* CreatedObject = nullptr;
		FTopLevelAssetPath RedirectorAssetPath;
		if (!FTopLevelAssetPath::TryCreate(
			RedirectorPath, RedirectorPath.GetPackageName(), RedirectorAssetPath))
			return Error(EAssetError::InvalidPath,
				"The redirector top-level asset path is invalid.");
		Result = CreateAsset(
			RedirectorAssetPath,
			DAssetRedirector::StaticClass(),
			sizeof(DAssetRedirector),
			CreatedObject);
		if (!Result) return Result;
		OutRedirector = Cast<DAssetRedirector>(CreatedObject);
		if (!OutRedirector)
			return Error(EAssetError::InvalidObjectGraph,
				"Failed to construct the redirector top-level asset.");
		OutRedirector->SetDestinationObject(DestinationObject);
		return {};
	}

	auto FAssetLoadService::LoadPackage(
		const FPackagePath& Path,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		OutPackage = nullptr;
		if (DPackage* Resident = FindResidentPackage(Path))
		{
			OutPackage = Resident;
			return {};
		}
		const FAssetCatalogEntry Entry = Durin::Asset::FindAssetExact(Path);
		if (!Entry)
			return Error(EAssetError::NotFound, std::format(
				"Package {} is not present in the registry.", Path.ToString()));
		return LoadPackageFromPhysicalPath(
			Path, Entry->PhysicalPath, OutPackage, OutReport);
	}

	auto FAssetLoadService::LoadObject(
		const FObjectPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		OutObject = nullptr;
		if (OutReport) *OutReport = {
			.RequestedPath = Path.GetPackagePath(),
			.PackagePath = Path.GetPackagePath()};
		auto Finish = [&](FAssetResult Result) -> FAssetResult
		{
			if (OutReport)
			{
				OutReport->Error = Result.Error;
				OutReport->ErrorMessage = Result.Message;
			}
			return Result;
		};
		if (!Path.IsValid())
			return Finish(Error(EAssetError::InvalidPath, "An object load requires an exact object path."));
		if (ExpectedClass && !ExpectedClass->IsChildOf(DObject::StaticClass()))
			return Finish(Error(EAssetError::TypeMismatch, "An object load requires a DObject class."));
		if (!bAcceptingRequests)
			return Finish(Error(EAssetError::ShuttingDown,
				"Object loading is closed while the asset manager is shutting down."));
		const FObjectPathResolveResult Resolution = Durin::Asset::ResolveObjectPath(
			Path, {.ExpectedClass = ExpectedClass});
		if (!Resolution)
		{
			if (Resolution.State == EAssetPathResolveState::NotFound)
			{
				DPackage* Resident = FindResidentPackage(Path.GetPackagePath());
				DObject* ResidentObject = Resident ? FindPackageObject(Resident, Path) : nullptr;
				if (ResidentObject && (!ExpectedClass || ResidentObject->IsA(ExpectedClass)))
				{
					OutObject = ResidentObject;
					return {};
				}
			}
			if (OutReport)
			{
				OutReport->CatalogRevision = Resolution.CatalogRevision;
				OutReport->FinalPath = Resolution.FinalPath.GetPackagePath();
			}
			return Finish(ObjectPathResolutionError(Resolution));
		}
		DPackage* Package = nullptr;
		FAssetResult Result = LoadPackage(
			Resolution.FinalPath.GetPackagePath(), Package, OutReport);
		if (!Result) return Finish(Result);
		DObject* Object = FindPackageObject(Package, Resolution.FinalPath);
		if (!Object)
			return Finish(Error(EAssetError::NotFound, std::format(
				"Object {} is not present in its loaded package.",
				Resolution.FinalPath.ToString())));
		if (ExpectedClass && !Object->IsA(ExpectedClass))
			return Finish(Error(EAssetError::TypeMismatch, std::format(
				"Object {} is not a {}.", Resolution.FinalPath.ToString(),
				ExpectedClass->GetQualifiedName().ToString())));
		if (OutReport)
		{
			OutReport->RequestedPath = Path.GetPackagePath();
			OutReport->FinalPath = Resolution.FinalPath.GetPackagePath();
			OutReport->PackagePath = Resolution.FinalPath.GetPackagePath();
			OutReport->CatalogRevision = Resolution.CatalogRevision;
			OutReport->RedirectChain.clear();
			for (const FObjectPath& Redirect : Resolution.RedirectChain)
				OutReport->RedirectChain.push_back(Redirect.GetPackagePath());
			if (Resolution.FinalAssetData)
				OutReport->FinalAssetClassName =
					Resolution.FinalAssetData->AssetClassName;
		}
		OutObject = Object;
		return Finish({});
	}

	auto FAssetLoadService::LoadPackageFromPhysicalPath(
		const FPackagePath& Path,
		std::string_view PhysicalPath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.Load");
		if (!bAcceptingRequests)
		{
			OutPackage = nullptr;
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
		if (bRootLoad) GActiveAssetLoadReport = PreviousLoadReport;
		--LoadDepth;
		if (bRootLoad)
		{
			if (!Result)
			{
				bool bDiscardedPackage = false;
				for (auto It = TransactionPackages.rbegin(); It != TransactionPackages.rend(); ++It)
				{
					DPackage* TransactionPackage = FindResidentPackage(*It);
					if (!TransactionPackage) continue;
					LoadingPackages.erase(*It);
					MarkObjectHierarchyAsGarbage(TransactionPackage);
					bDiscardedPackage = true;
				}
				if (bDiscardedPackage) CollectGarbage();
				if (OutReport) *OutReport = std::move(FailureReport);
			}
			TransactionPackages.clear();
		}
		OutPackage = Result ? Package : nullptr;
		return Result;
	}

	auto FAssetLoadService::LoadPackageInternal(
		const FPackagePath& Path,
		std::string_view PhysicalPath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.LoadPackage");
		if (DPackage* Resident = FindResidentPackage(Path))
		{
			OutPackage = Resident;
			return {};
		}
		FAssetLoadReport LocalReport{.PackagePath = Path};
		FAssetLoadReport* CodecReport = OutReport ? OutReport : &LocalReport;
		std::vector<std::byte> Bytes;
		if (PhysicalPath.empty()) return Error(EAssetError::InvalidPath, "Asset path cannot be resolved in the selected package mode.");
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath)) return Error(EAssetError::NotFound, std::format("Asset {} was not found.", Path.ToString()));
		++GActivePackageFileReadCount;
		std::filesystem::path BulkPath(PhysicalPath);
		BulkPath.replace_extension(".dbulk");
		std::error_code BulkError;
		uint64 PhysicalBulkBytes = 0;
		if (std::filesystem::is_regular_file(BulkPath, BulkError))
			PhysicalBulkBytes = std::filesystem::file_size(BulkPath, BulkError);
		if (BulkError && BulkError != std::errc::no_such_file_or_directory)
			return Error(EAssetError::IoError,
				std::format("Asset {} bulk companion could not be inspected.", Path.ToString()));
		const Private::FAssetPackageReadContext HeaderContext{
			.PackageBytes = Bytes, .PackagePath = Path,
			.PhysicalPackageBytes = Bytes.size(),
			.PhysicalBulkBytes = PhysicalBulkBytes,
			.bResourceBackedBulk = true,
			.bCooked = RuntimeConfiguration.IsCooked()};
		const Private::FAssetPackageCodec* Codec = nullptr;
		if (FAssetResult Result = Private::ResolveAssetPackageReader(
				Bytes, Codec); !Result)
			return Result;
		{
			FAssetPackageHeader Header;
			FAssetResult Result = Codec->ReadHeader(HeaderContext, Header);
			if (!Result) return Result;
			const Private::FAssetPackageReadContext ReadContext{
				.PackageBytes = Bytes, .PackagePath = Path,
				.PhysicalPackageBytes = Bytes.size(),
				.PhysicalBulkBytes = PhysicalBulkBytes,
				.bResourceBackedBulk = true,
				.bCooked = RuntimeConfiguration.IsCooked()};
			const Private::FMutationPackageMetadata HeaderMetadata{
				.FormatVersion = Header.FormatVersion,
				.AssetClassName = Header.AssetClassName,
				.EntryKind = Header.EntryKind,
				.RedirectDestination = Header.RedirectDestination,
				.Dependencies = Header.Dependencies};
			Result = Private::ValidateMutationPackageMetadata(
				HeaderMetadata, Header.ObjectCount, &Path);
			if (!Result) return Result;

			bool bRegisteredBulkResource = false;
			if (Header.BulkSegmentExtent != 0)
			{
				FAssetPackageInspection Inspection;
				Result = Codec->Inspect(ReadContext, Inspection);
				if (!Result) return Result;
				std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
				std::string BulkDiagnostic;
				if (!InspectEditorBulkDataStorageDescriptors(
						Inspection, Descriptors, &BulkDiagnostic))
					return Error(EAssetError::CorruptFile, std::move(BulkDiagnostic));
				std::vector<FPackageBulkDataEntry> Entries;
				Entries.reserve(Descriptors.size());
				for (size_t Index = 0; Index < Descriptors.size(); ++Index)
				{
					const auto& Descriptor = Descriptors[Index];
					Entries.push_back({
						.FieldIndex = Index + 1,
						.Placement = Descriptor.StorageKind
							== EEditorBulkDataStorageKind::External
							? EPackageBulkDataPlacement::External
							: EPackageBulkDataPlacement::Inline,
						.LogicalSize = Descriptor.LogicalByteCount,
						.StoredSize = Descriptor.StoredByteCount,
						.SegmentOffset = Descriptor.SegmentOffset,
						.Alignment = Descriptor.Alignment,
						.ContentId = Descriptor.ContentHash});
				}
				FPackageResourceHandle Resource;
				if (!GetPackageResourceManager().RegisterLoosePackage(
						Path.ToString(), std::filesystem::path(PhysicalPath),
						{Header.BulkSegmentExtent, Header.BulkSegmentDigest},
						Entries, Resource, &BulkDiagnostic))
					return Error(EAssetError::CorruptFile, std::move(BulkDiagnostic));
				bRegisteredBulkResource = true;
			}
			DPackage* Package = nullptr;
			Result = Codec->Load(
				ReadContext, Package, CodecReport,
				[&](DPackage* LoadedPackage) -> FAssetResult {
					if (!LoadedPackage
						|| FindPackage(Path.GetView()) != LoadedPackage
						|| LoadingPackages.contains(Path))
						return Error(EAssetError::AlreadyExists,
							"The package skeleton is already resident.");
					LoadingPackages.insert(Path);
					if (LoadDepth > 0) TransactionPackages.push_back(Path);
					return {};
				},
				[&](DPackage* LoadedPackage) {
					LoadingPackages.erase(Path);
				});
			CodecReport->PackageFileReadCount = GActivePackageFileReadCount;
			if (!Result)
			{
				if (bRegisteredBulkResource)
					GetPackageResourceManager().RetirePackage(Path.ToString());
				return Result;
			}
			LoadingPackages.erase(Path);
			OutPackage = Package;
			return {};
		}
	}

	auto FAssetLoadService::FindResidentPackage(const FPackagePath& Path) const -> DPackage*
	{
		DPackage* Package = FindPackage(Path.GetView());
		return Package && !Package->IsGarbage() && Package->IsAssetPackage()
			? Package : nullptr;
	}

	auto FAssetLoadService::IsPackageReferenced(const DPackage* Package) const -> bool
	{
		if (!Package) return false;
		FPackagePath Path;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path)) return false;
		for (DPackage* OtherPackage : GetResidentAssetPackages())
		{
			if (OtherPackage == Package) continue;
			FPackagePath OtherPath;
			if (!FPackagePath::TryCreate(OtherPackage->GetPackagePath(), OtherPath))
				continue;
			const FAssetCatalogEntry Data = FindAssetExact(OtherPath);
			if (!Data) continue;
			for (const FPackagePath& Dependency : Data->Dependencies)
			{
				const FAssetPathResolveResult Resolution = Durin::Asset::ResolveAssetPath(Dependency);
				if (Resolution && Resolution.FinalPath == Path) return true;
			}
		}
		return false;
	}

	auto FAssetLoadService::UnloadPackage(
		const FPackagePath& Path,
		EAssetPackageUnloadPolicy Policy) -> FAssetResult
	{
		DPackage* Package = FindResidentPackage(Path);
		if (!Package)
			return Error(EAssetError::NotFound, "Package is not resident.");
		if (LoadingPackages.contains(Path) || IsPackageReferenced(Package))
			return Error(EAssetError::InUse, "Package is still referenced.");
		const bool bHasUnsavedState =
			Package->IsNewlyCreated() || Package->IsDirty();
		if (bHasUnsavedState
			&& Policy == EAssetPackageUnloadPolicy::RejectUnsaved)
			return Error(EAssetError::InUse,
				"Package has unsaved state; explicit discard policy is required.");
		Package->SetStandaloneResidency(false);
		CollectGarbage();
		if (DPackage* RemainingPackage = FindResidentPackage(Path))
		{
			RemainingPackage->SetStandaloneResidency(true);
			return Error(EAssetError::InUse,
				"Package remains referenced by live objects.");
		}
		GetPackageResourceManager().RetirePackage(Path.ToString());
		InvalidateSoftObjectCaches();
		return {};
	}

	auto FAssetLoadService::CapturePackageLoadSnapshot() const -> FAssetPackageLoadSnapshot
	{
		FAssetPackageLoadSnapshot Snapshot;
		for (DPackage* Package : GetResidentAssetPackages())
		{
			FPackagePath Path;
			if (FPackagePath::TryCreate(Package->GetPackagePath(), Path))
				Snapshot.ResidentPackages.push_back(std::move(Path));
		}
		std::ranges::sort(Snapshot.ResidentPackages, {}, [](const FPackagePath& Path) {
			return Path.ToString();
		});
		return Snapshot;
	}

	auto FAssetLoadService::ReleasePackagesLoadedSince(
		const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult
	{
		if (LoadDepth != 0 || !LoadingPackages.empty())
			return Error(EAssetError::InUse, "A package load is still in progress.");

		std::unordered_set<FPackagePath> Protected(
			Snapshot.ResidentPackages.begin(), Snapshot.ResidentPackages.end());
		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;
			for (DPackage* Package : GetResidentAssetPackages())
			{
				FPackagePath Path;
				if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path)) continue;
				if (!Protected.contains(Path)) continue;
				const FAssetCatalogEntry Data = FindAssetExact(Path);
				if (!Data) continue;
				for (const FPackagePath& Dependency : Data->Dependencies)
				{
					const FAssetPathResolveResult Resolution = Durin::Asset::ResolveAssetPath(Dependency);
					if (Resolution) bChanged |= Protected.insert(Resolution.FinalPath).second;
				}
			}
		}

		std::vector<DPackage*> ReleasedPackages;
		std::vector<FPackagePath> ReleasedPaths;
		for (DPackage* Package : GetResidentAssetPackages())
		{
			FPackagePath Path;
			if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path)
				|| Protected.contains(Path)
				|| Package->IsNewlyCreated()
				|| Package->IsDirty()) continue;
			ReleasedPackages.push_back(Package);
			ReleasedPaths.push_back(std::move(Path));
		}
		for (const FPackagePath& Path : ReleasedPaths)
			GetPackageResourceManager().RetirePackage(Path.ToString());
		for (DPackage* Package : ReleasedPackages)
		{
			MarkObjectHierarchyAsGarbage(Package);
		}
		if (!ReleasedPackages.empty()) CollectGarbage();
		return {};
	}

	auto FAssetRuntimeState::Shutdown() -> void
	{
		StopAcceptingRequests();
		FlushAssetRegistryCaches();
		GetPackageResourceManager().RetireAllPackages();
		std::vector<DPackage*> Packages = GetResidentAssetPackages();
		Loader.Reset();
		for (DPackage* Package : Packages)
		{
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
		check(GetResidentAssetPackages().empty());
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

		const FObjectPath& Path = Reference.GetPath();
		const FObjectPathResolveResult Resolution = Durin::Asset::ResolveObjectPath(
			Path, {.ExpectedClass = ExpectedClass});
		if (!Resolution)
		{
			if (Resolution.State == EAssetPathResolveState::NotFound)
			{
				DPackage* LoadedPackage = FindResidentPackage(Path.GetPackagePath());
				DObject* LoadedObject = LoadedPackage
					? FindPackageObject(LoadedPackage, Path) : nullptr;
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
						LoadedObject, Reference.GetPath(), Reference.GetPath(),
						ExpectedClass, &ValidationError))
						return {
							.Result = Error(EAssetError::InvalidObjectGraph, std::move(ValidationError)),
							.State = ESoftObjectResolveState::NotLoaded};
					return {
						.State = ESoftObjectResolveState::Loaded,
						.Object = LoadedObject,
						.ResolvedPath = Path};
				}
			}
			Reference.ResetCache();
			return {
				.Result = ObjectPathResolutionError(Resolution),
				.State = ESoftObjectResolveState::NotLoaded};
		}

		DPackage* Package = FindResidentPackage(
			Resolution.FinalPath.GetPackagePath());
		if (!Package)
		{
			Reference.ResetCache();
			return {
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}

		DObject* Object = FindPackageObject(Package, Resolution.FinalPath);
		if (!Object)
		{
			return {
				.Result = Error(EAssetError::InvalidObjectGraph, std::format(
					"Loaded package {} has no object {}.",
					Resolution.FinalPath.GetPackagePath().ToString(),
					Resolution.FinalPath.ToString())),
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
			Object, Reference.GetPath(), Resolution.FinalPath,
			ExpectedClass, &ValidationError))
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
		const FObjectPath& ResolvedObjectPath = Resolved.ResolvedPath.IsValid()
			? Resolved.ResolvedPath : Reference.GetPath();
		FAssetResult Result = LoadObject(
			ResolvedObjectPath, ExpectedClass, LoadedObject, OutReport);
		if (!Result) return Result;

		std::string ValidationError;
		if (!Reference.TrySetResolvedObject(
			LoadedObject, Reference.GetPath(), ResolvedObjectPath,
			ExpectedClass, &ValidationError))
			return Error(EAssetError::InvalidObjectGraph, std::move(ValidationError));
		OutObject = LoadedObject;
		return {};
	}

}
