#include "DObject/PackagePersistence.h"
#include "Asset/RegistryOperations.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetLiveLoadGuard.h"
#include "AssetRegistry/Scan.h"
#include "AssetMutationRegistryInternal.h"
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
#include "DObject/PackageValueCodec.h"
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

namespace Durin::AssetPrivate
{
	thread_local FAssetLiveLoadGuard* FAssetLiveLoadGuard::Active = nullptr;
	std::atomic_uint64_t FAssetLiveLoadGuard::ActiveCount = 0;
	std::atomic_uint64_t FAssetLiveLoadGuard::Rejections = 0;

	FAssetLiveLoadGuard::FAssetLiveLoadGuard(bool bInEnabled) : bEnabled(bInEnabled)
	{
		if (bEnabled) { InitialRejections = Rejections.load(); ++ActiveCount; Previous = std::exchange(Active, this); }
	}

	FAssetLiveLoadGuard::~FAssetLiveLoadGuard()
	{
		if (bEnabled) { Active = Previous; --ActiveCount; }
	}

	auto FAssetLiveLoadGuard::GetFailure() const -> FAssetResult
	{
		if (Failure && bEnabled && Rejections.load() != InitialRejections)
			return {EAssetError::InUse, "A live operation was rejected during input capture."};
		return Failure;
	}

	auto FAssetLiveLoadGuard::Check(std::string_view Operation, std::string_view Path) -> FAssetResult
	{
		if (ActiveCount.load() == 0) return {};
		++Rejections;
		const FAssetResult Result{EAssetError::InUse,
			std::format("Implicit live {} for '{}' is forbidden during guarded package loading.", Operation, Path)};
		for (auto* Guard = Active; Guard; Guard = Guard->Previous)
			if (Guard->Failure) Guard->Failure = Result;
		return Result;
	}
}

namespace Durin
{
	using AssetPrivate::AssetReferenceLess;
	using AssetPrivate::EAssetMutationState;
	using AssetPrivate::FAssetMutationJournal;
	using AssetPrivate::FAssetMutationJournalEntry;
	using AssetPrivate::FAssetReferenceStoreRegistry;
	using AssetPrivate::FingerprintRelocationFile;
	using AssetPrivate::GetAssetReferenceStoreRegistry;
	using AssetPrivate::LoadRelocationBytes;
	using AssetPrivate::MakePackageFingerprint;
	using AssetPrivate::NormalizePhysicalPath;
	using AssetPrivate::PublishRelocationFile;
	using AssetPrivate::RecoverPendingMutationJournals;
	using AssetPrivate::SaveRelocationBytes;
	using AssetPrivate::WriteMutationJournalState;

	namespace
	{
		thread_local FAssetLoadReport* GActiveAssetLoadReport = nullptr;
		thread_local uint64 GActivePackageFileReadCount = 0;
		thread_local std::vector<TWeakObjectPtr<DPackage>>* GOwnedLoadPackages = nullptr;

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
			FByteView Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult;

		auto ProjectionPendingError(const FPackagePath& Path) -> FAssetResult
		{
			return {EAssetError::StaleData,
				std::format("Registry projection for package {} is pending synchronization.", Path.ToString()),
				EAssetResultDisposition::ContentCommittedProjectionPending};
		}

		auto ObjectPathResolutionError(
			const FObjectPathResolveResult& Resolution) -> FAssetResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::ProjectionPending:
				return ProjectionPendingError(Resolution.FinalPath.GetPackagePath());
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
				if (Durin::FindResidentPackage(Package->GetPackagePathIdentity()) == Package)
					Packages.push_back(Package);
			}
			return Packages;
		}
	}

	auto FAssetPackageLoadScope::LoadPackage(const FPackagePath& Path,
		DPackage*& OutPackage, FAssetLoadReport* OutReport) -> FAssetResult
	{
		CheckSoftObjectThread();
		if (!FAssetRuntimeState::Get().GetLoadService().IsIdle())
		{
			OutPackage = nullptr;
			return Error(EAssetError::InUse, "A load scope requires a top-level load invocation.");
		}
		struct FRestoreLoadOwner
		{
			std::vector<TWeakObjectPtr<DPackage>>* Previous;
			~FRestoreLoadOwner() { GOwnedLoadPackages = Previous; }
		} Restore{std::exchange(GOwnedLoadPackages, &Packages)};
		return Durin::LoadPackage(Path, OutPackage, OutReport);
	}

	auto FAssetPackageLoadScope::LoadObject(const FObjectPath& Path, const DClass* ExpectedClass,
		DObject*& OutObject, FAssetLoadReport* OutReport) -> FAssetResult
	{
		CheckSoftObjectThread();
		if (!FAssetRuntimeState::Get().GetLoadService().IsIdle())
		{
			OutObject = nullptr;
			return Error(EAssetError::InUse, "A load scope requires a top-level load invocation.");
		}
		struct FRestoreLoadOwner
		{
			std::vector<TWeakObjectPtr<DPackage>>* Previous;
			~FRestoreLoadOwner() { GOwnedLoadPackages = Previous; }
		} Restore{std::exchange(GOwnedLoadPackages, &Packages)};
		return Durin::LoadObject(Path, ExpectedClass, OutObject, OutReport);
	}

	auto FAssetPackageLoadScope::LoadSoftObject(FSoftObjectPtr& Reference, const DClass* ExpectedClass,
		DObject*& OutObject, ESoftObjectNullPolicy NullPolicy, FAssetLoadReport* OutReport) -> FAssetResult
	{
		CheckSoftObjectThread();
		if (!FAssetRuntimeState::Get().GetLoadService().IsIdle())
		{
			OutObject = nullptr;
			return Error(EAssetError::InUse, "A load scope requires a top-level load invocation.");
		}
		struct FRestoreLoadOwner
		{
			std::vector<TWeakObjectPtr<DPackage>>* Previous;
			~FRestoreLoadOwner() { GOwnedLoadPackages = Previous; }
		} Restore{std::exchange(GOwnedLoadPackages, &Packages)};
		return Durin::LoadSoftObject(Reference, ExpectedClass, OutObject, NullPolicy, OutReport);
	}

	auto FAssetPackageLoadScope::Release(
		std::span<const TWeakObjectPtr<DPackage>> IgnoreSavedDependencies) -> FAssetResult
	{
		CheckSoftObjectThread();
		const FAssetResult Result = FAssetRuntimeState::Get().GetLoadService().ReleasePackages(
			Packages, IgnoreSavedDependencies);
		std::erase_if(Packages, [](const auto& Package) { return !Package.IsValid(); });
		return Result;
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
		: Loader(RuntimeConfiguration, bAcceptingRequests)
		, Mutations(
			Loader,
			RuntimeConfiguration,
			bAcceptingRequests)
	{
	}

	auto FAssetLoadService::LoadPackage(
		const FPackagePath& Path,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		OutPackage = nullptr;
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("package load", Path.ToString()); !Result)
		{
			if (OutReport) *OutReport = {.RequestedPath = Path, .PackagePath = Path,
				.Error = Result.Error, .ErrorMessage = Result.Message};
			return Result;
		}
		if (IsAssetRegistryProjectionFenced(Path))
		{
			const FAssetResult Result = ProjectionPendingError(Path);
			if (OutReport) *OutReport = {.RequestedPath = Path, .FinalPath = Path,
				.PackagePath = Path, .Error = Result.Error, .ErrorMessage = Result.Message};
			return Result;
		}
		if (IsPackageLoading(Path))
			return Error(EAssetError::InUse, std::format("Package '{}' is not ready for public loading.", Path.ToString()));
		if (DPackage* Resident = FindResidentPackage(Path))
		{
			OutPackage = Resident;
			return {};
		}
		const FAssetCatalogEntry Entry = Durin::FindAssetExact(Path);
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
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("object load", Path.ToString()); !Result)
			return Finish(Result);
		if (!Path.IsValid())
			return Finish(Error(EAssetError::InvalidPath, "An object load requires an exact object path."));
		if (ExpectedClass && !ExpectedClass->IsChildOf(DObject::StaticClass()))
			return Finish(Error(EAssetError::TypeMismatch, "An object load requires a DObject class."));
		if (!bAcceptingRequests)
			return Finish(Error(EAssetError::ShuttingDown,
				"Object loading is closed while the asset manager is shutting down."));
		const FObjectPathResolveResult Resolution = Durin::ResolveAssetObjectPathForOperation(
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
				ExpectedClass->GetQualifiedName())));
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
		OutPackage = nullptr;
		const bool bRootLoad = LoadDepth++ == 0;
		if (bRootLoad) GActivePackageFileReadCount = 0;
		FAssetLoadReport* PreviousLoadReport = GActiveAssetLoadReport;
		if (bRootLoad) GActiveAssetLoadReport = OutReport;
		struct FRestoreRequest
		{
			uint32& Depth;
			FAssetLoadReport* Previous;
			~FRestoreRequest() { --Depth; GActiveAssetLoadReport = Previous; }
		} RequestScope{LoadDepth, PreviousLoadReport};
		auto Record = std::make_shared<FPendingLoad>();
		Record->Path = Path;
		Record->Index = Record->LowLink = NextLoadIndex++;
		bool bCompleted = false;
		struct FDiscardScope
		{
			std::function<void()> Cleanup;
			~FDiscardScope() { Cleanup(); }
		} CandidateScope{[&] { if (!bCompleted) { DiscardIncomplete(Record->Index); PendingLoads.erase(Path); } }};
		FAssetResult Result;
		try
		{
			PendingLoads.emplace(Path, Record);
			CompletionStack.push_back(Record);
			DPackage* Package = nullptr;
			Result = LoadPackageInternal(Path, PhysicalPath, Package, OutReport);
			if (Result && Record->LowLink == Record->Index) Result = CompleteComponent(*Record);
			if (Result)
			{
				bCompleted = true;
				OutPackage = Package; // Only loader bindings can observe a pending component.
			}
		}
		catch (const std::exception& Exception)
		{
			Result = Error(EAssetError::InvalidObjectGraph,
				std::format("Package '{}': load callback failed: {}", Path.ToString(), Exception.what()));
		}
		catch (...)
		{
			Result = Error(EAssetError::InvalidObjectGraph,
				std::format("Package '{}': load callback threw an exception.", Path.ToString()));
		}
		if (OutReport)
		{
			OutReport->Error = Result.Error;
			OutReport->ErrorMessage = Result.Message;
			OutReport->PackageFileReadCount = GActivePackageFileReadCount;
		}
		return Result;
	}

	auto FAssetLoadService::ResolveDependencyPackage(FPendingLoad& Owner,
		const FPackagePath& Path, DPackage*& OutPackage) -> FAssetResult
	{
		OutPackage = nullptr;
		if (IsAssetRegistryProjectionFenced(Path)) return ProjectionPendingError(Path);
		if (const auto It = PendingLoads.find(Path); It != PendingLoads.end())
		{
			const auto& Target = *It->second;
			if (!Target.Package || Target.Phase == ELoadPhase::PostLoading || Target.Phase == ELoadPhase::Failed)
				return Error(EAssetError::InUse, std::format("Dependency '{}' cannot join this load component.", Path.ToString()));
			Owner.LowLink = std::min(Owner.LowLink, Target.Index);
			OutPackage = Target.Package.Get();
			return {};
		}
		if (DPackage* Resident = FindResidentPackage(Path))
		{
			OutPackage = Resident;
			return {};
		}
		const auto Entry = Durin::FindAssetExact(Path);
		if (!Entry) return Error(EAssetError::MissingDependency,
			std::format("Dependency '{}' is not present in the registry.", Path.ToString()));
		auto Result = LoadPackageFromPhysicalPath(Path, Entry->PhysicalPath, OutPackage);
		if (Result)
			if (const auto It = PendingLoads.find(Path); It != PendingLoads.end())
				Owner.LowLink = std::min(Owner.LowLink, It->second->LowLink);
		return Result;
	}

	auto FAssetLoadService::ResolveDependencyObject(FPendingLoad& Owner,
		const FObjectPath& Path, DObject*& OutObject) -> FAssetResult
	{
		OutObject = nullptr;
		const auto Resolution = Durin::ResolveAssetObjectPathForOperation(Path);
		if (!Resolution) return ObjectPathResolutionError(Resolution);
		DPackage* Package = nullptr;
		if (auto Result = ResolveDependencyPackage(Owner, Resolution.FinalPath.GetPackagePath(), Package); !Result)
			return Result;
		OutObject = FindPackageObject(Package, Resolution.FinalPath);
		return OutObject ? FAssetResult{} : Error(EAssetError::MissingDependency,
			std::format("Dependency object '{}' is absent from its package.", Resolution.FinalPath.ToString()));
	}

	auto FAssetLoadService::CompleteComponent(FPendingLoad& Root) -> FAssetResult
	{
		// Pending DFS suffix is exactly this strongly connected component. Copy it
		// because PostLoad may synchronously complete an independent component.
		std::vector<std::shared_ptr<FPendingLoad>> Group;
		for (auto It = CompletionStack.rbegin(); It != CompletionStack.rend(); ++It)
		{
			Group.push_back(*It);
			if (It->get() == &Root) break;
		}
		for (const auto& Member : Group)
		{
			if (Member->Phase != ELoadPhase::ValuesRestored || !Member->Validate || !Member->PostLoad)
				return Error(EAssetError::InvalidObjectGraph, "Load component contains unrestored values.");
			if (auto Result = Member->Validate(); !Result) return Result;
			Member->Phase = ELoadPhase::Validated;
		}
		// Allocate the explicit operation's ownership entries before notifications.
		if (GOwnedLoadPackages)
		{
			GOwnedLoadPackages->reserve(GOwnedLoadPackages->size() + Group.size());
			for (const auto& Member : Group) GOwnedLoadPackages->emplace_back(Member->Package.Get());
		}
		for (const auto& Member : Group) Member->Phase = ELoadPhase::PostLoading;
		for (const auto& Member : Group) Member->PostLoad();
		for (const auto& Member : Group)
		{
			Member->Phase = ELoadPhase::Ready;
			Member->bOwnResource = false;
			PendingLoads.erase(Member->Path);
			CompletionStack.pop_back();
		}
		return {};
	}

	auto FAssetLoadService::DiscardIncomplete(uint64 FirstIndex) noexcept -> void
	{
		// Hide every candidate first. Physical retirement can run object callbacks.
		for (const auto& Record : CompletionStack)
			if (Record->Index >= FirstIndex)
			{
				Record->Phase = ELoadPhase::Failed;
				if (Record->Package) MarkObjectHierarchyAsGarbage(Record->Package.Get());
			}
		while (!CompletionStack.empty() && CompletionStack.back()->Index >= FirstIndex)
		{
			auto Record = std::move(CompletionStack.back());
			CompletionStack.pop_back();
			PendingLoads.erase(Record->Path);
			if (Record->bOwnResource)
			{
				Record->bOwnResource = false;
				try { GetPackageResourceManager().RetirePackage(Record->Path.ToString()); }
				catch (...) { /* Do not mask the original load failure. */ }
			}
			Record->Package.Reset();
		}
		try { CollectGarbage(); }
		catch (...) { /* Object retirement must not replace the original failure. */ }
	}

	auto FAssetLoadService::LoadPackageInternal(
		const FPackagePath& Path,
		std::string_view PhysicalPath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.LoadPackage");
		auto ReadAccess = FPackageFileAccess::TryReadPackage(std::filesystem::path(PhysicalPath));
		if (!ReadAccess) return Error(EAssetError::InUse, "Package output is being written.");
		if (DPackage* Resident = FindResidentPackage(Path))
		{
			OutPackage = Resident;
			return {};
		}
		FAssetLoadReport LocalReport{.PackagePath = Path};
		FAssetLoadReport* CodecReport = OutReport ? OutReport : &LocalReport;
		FByteBuffer Bytes;
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
		const AssetPrivate::FAssetPackageReadContext HeaderContext{
			.PackageBytes = Bytes, .PackagePath = Path,
			.PhysicalPackageBytes = Bytes.size(),
			.PhysicalBulkBytes = PhysicalBulkBytes,
			.bResourceBackedBulk = true,
			.bCooked = RuntimeConfiguration.IsCooked()};
		const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
		if (FAssetResult Result = AssetPrivate::ResolveAssetPackageReader(
				Bytes, Codec); !Result)
			return Result;
		{
			FAssetPackageHeader Header;
			FAssetResult Result = Codec->ReadHeader(HeaderContext, Header);
			if (!Result) return Result;
			AssetPrivate::FAssetPackageReadContext ReadContext{
				.PackageBytes = Bytes, .PackagePath = Path,
				.PhysicalPackageBytes = Bytes.size(),
				.PhysicalBulkBytes = PhysicalBulkBytes,
				.bResourceBackedBulk = true,
				.bCooked = RuntimeConfiguration.IsCooked()};
			const AssetPrivate::FMutationPackageMetadata HeaderMetadata{
				.FormatVersion = Header.FormatVersion,
				.TopLevelAssets = Header.TopLevelAssets,
				.AssetClassName = Header.AssetClassName,
				.EntryKind = Header.EntryKind,
				.RedirectDestination = Header.RedirectDestination,
				.Dependencies = Header.Dependencies};
			Result = AssetPrivate::ValidateMutationPackageMetadata(
				HeaderMetadata, Header.ObjectCount, &Path);
			if (!Result) return Result;

			auto& Record = *PendingLoads.at(Path);
			if (Header.BulkSegmentExtent != 0)
			{
				FAssetPackageInspection Inspection;
				Result = Codec->Inspect(ReadContext, Inspection);
				if (!Result) return Result;
				std::vector<FPackageBulkStorageDescriptor> Descriptors;
				if (const auto Storage = InspectEditorBulkDataStorageDescriptors(Inspection, Descriptors); !Storage)
					return {.Error = EAssetError::CorruptFile, .Message = FormatEditorBulkDataStorageError(Storage.Error),
						.BulkStorageCause = Storage.Error};
				std::vector<FPackageBulkDataEntry> Entries;
				Entries.reserve(Descriptors.size());
				for (size_t Index = 0; Index < Descriptors.size(); ++Index)
				{
					const auto& Descriptor = Descriptors[Index];
					Entries.push_back({
						.FieldIndex = Index + 1,
						.Placement = Descriptor.StorageKind
							== EPackageBulkStorageKind::External
							? EPackageBulkDataPlacement::External
							: EPackageBulkDataPlacement::Inline,
						.LogicalSize = Descriptor.LogicalByteCount,
						.StoredSize = Descriptor.StoredByteCount,
						.SegmentOffset = Descriptor.SegmentOffset,
						.Alignment = Descriptor.Alignment,
						.ContentId = Descriptor.ContentHash});
				}
				auto Registration = GetPackageResourceManager().RegisterLoosePackage(
					Path.ToString(), std::filesystem::path(PhysicalPath),
					{Header.BulkSegmentExtent, Header.BulkSegmentDigest},
					Entries
				);
				if (!Registration)
				{
					const EAssetError Code = Registration.Error.Code == EPackageResourceRegistrationError::ShuttingDown ? EAssetError::ShuttingDown : Registration.Error.PublicationError.Operation != FFileHelper::EAtomicFileOperation::None ? EAssetError::IoError :
																																																										EAssetError::CorruptFile;
					return {.Error = Code, .Message = FormatPackageResourceRegistrationError(Registration.Error),
						.ResourceRegistrationCause = std::move(Registration.Error)};
				}
				Record.bOwnResource = true;
				ReadContext.BulkResource = std::move(Registration.Resource);
			}
			ReadContext.DependencyLoadPolicy = AssetPrivate::FAssetPackageDependencyLoadPolicy{
				.ResolvePackage = [this, &Record](const FPackagePath& Dependency, DPackage*& Out) {
					return ResolveDependencyPackage(Record, Dependency, Out);
				},
				.ResolveObject = [this, &Record](const FObjectPath& Object, DObject*& Out) {
					return ResolveDependencyObject(Record, Object, Out);
				},
			};
			ReadContext.DeferCompletion = [&Record](std::function<FAssetResult()> Validate, std::function<void()> Notify) {
				Record.Validate = std::move(Validate);
				Record.PostLoad = std::move(Notify);
				Record.Phase = ELoadPhase::ValuesRestored;
			};
			DPackage* Package = nullptr;
			Result = Codec->Load(
				ReadContext, Package, CodecReport,
				[&](DPackage* LoadedPackage) -> FAssetResult {
					if (!LoadedPackage || FindPackage(Path.GetView()) != LoadedPackage || Record.Package)
						return Error(EAssetError::AlreadyExists, "The package skeleton is already resident.");
					Record.Package = LoadedPackage;
					Record.Phase = ELoadPhase::Skeleton;
					return {};
				},
				{});
			CodecReport->PackageFileReadCount = GActivePackageFileReadCount;
			if (!Result) return Result;
			OutPackage = Package;
			return {};
		}
	}

	auto FAssetLoadService::FindResidentPackage(const FPackagePath& Path) const -> DPackage*
	{
		if (IsPackageLoading(Path)) return nullptr;
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
				const FAssetPathResolveResult Resolution = Durin::ResolveAssetPathForOperation(Dependency);
				if (Resolution && Resolution.FinalPath == Path) return true;
			}
		}
		return false;
	}

	auto FAssetLoadService::UnloadPackage(
		const FPackagePath& Path,
		EAssetPackageUnloadPolicy Policy) -> FAssetResult
	{
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("UnloadPackage", ""); !Result) return Result;
		DPackage* Package = FindResidentPackage(Path);
		if (!Package)
			return Error(EAssetError::NotFound, "Package is not resident.");
		if (IsPackageLoading(Path) || IsPackageReferenced(Package))
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

	auto FAssetLoadService::ReleasePackages(
		std::span<const TWeakObjectPtr<DPackage>> Packages,
		std::span<const TWeakObjectPtr<DPackage>> IgnoreSavedDependencies) -> FAssetResult
	{
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("ReleasePackages", ""); !Result) return Result;
		if (LoadDepth != 0 || !PendingLoads.empty())
			return Error(EAssetError::InUse, "A package load is still in progress.");
		std::unordered_set<FPackagePath> Candidates;
		bool bInUse = false;
		for (const auto& Handle : Packages)
		{
			DPackage* Package = Handle.Get();
			if (!Package || FindResidentPackage(Package->GetPackagePathIdentity()) != Package) continue;
			if (Package->IsNewlyCreated() || Package->IsDirty())
			{
				bInUse = true;
				continue;
			}
			Candidates.insert(Package->GetPackagePathIdentity());
		}
		// Keep the disk dependency closure of every package outside the release set,
		// including dirty/new packages. Internal cycles can be collected as one batch.
		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;
			for (DPackage* Package : GetResidentAssetPackages())
			{
				const FPackagePath& Path = Package->GetPackagePathIdentity();
				if (Candidates.contains(Path)) continue;
				if (std::ranges::any_of(IgnoreSavedDependencies,
					[&](const auto& Handle) { return Handle.Get() == Package; })) continue;
				const FAssetCatalogEntry Data = FindAssetExact(Path);
				if (!Data) continue;
				for (const FPackagePath& Dependency : Data->Dependencies)
				{
					const FAssetPathResolveResult Resolution = Durin::ResolveAssetPathForOperation(Dependency);
					if (Resolution && Candidates.erase(Resolution.FinalPath))
					{
						bChanged = true;
						bInUse = true;
					}
				}
			}
		}
		for (const FPackagePath& Path : Candidates)
			FindResidentPackage(Path)->SetStandaloneResidency(false);
		if (!Candidates.empty()) CollectGarbage();
		bool bReleased = false;
		for (const FPackagePath& Path : Candidates)
		{
			if (DPackage* Remaining = FindResidentPackage(Path))
			{
				Remaining->SetStandaloneResidency(true);
				bInUse = true;
			}
			else
			{
				GetPackageResourceManager().RetirePackage(Path.ToString());
				bReleased = true;
			}
		}
		if (bReleased) InvalidateSoftObjectCaches();
		return bInUse ? Error(EAssetError::InUse,
			"Packages remain referenced or have unsaved state.") : FAssetResult{};
	}

	auto FAssetRuntimeState::Shutdown() -> void
	{
		if (!AssetPrivate::FAssetLiveLoadGuard::Check("Shutdown", "")) return;
		StopAcceptingRequests();
		PackageSavePrivate::SetAsyncSaveAdmission(false);
		if (auto Drain = DPackage::DrainAsyncSaves(); !Drain)
		{ DURIN_ERROR("Asset shutdown could not drain saves: {}", Drain.Message); return; }
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
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("Initialize", ""); !Result) return Result;
		if (bAcceptingRequests)
		{
			if (RuntimeConfiguration == Configuration) return {};
			return Error(EAssetError::InUse,
				"Asset runtime configuration cannot be replaced while Engine Asset is initialized.");
		}
		check(GetResidentAssetPackages().empty());
		check(Loader.IsIdle());
		RuntimeConfiguration = std::move(Configuration);
		if (!RuntimeConfiguration.IsCooked())
		{
			FAssetResult RecoveryResult = RecoverPendingMutationJournals();
			if (!RecoveryResult) return RecoveryResult;
		}
		PackageSavePrivate::SetAsyncSaveAdmission(true);
		bAcceptingRequests = true;
		return {};
	}

	auto FAssetRuntimeState::StopAcceptingRequests() -> void
	{
		if (!AssetPrivate::FAssetLiveLoadGuard::Check("StopAcceptingRequests", "")) return;
		if (!bAcceptingRequests) return;
		bAcceptingRequests = false;
		DURIN_DEBUG("Asset manager stopped accepting new requests.");
	}

	auto FAssetLoadService::ResolveSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult
	{
		if (!Reference.IsNull())
			if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check(
				"soft-object resolve", Reference.GetPath().ToString()); !Result)
				return {.Result = Result, .State = ESoftObjectResolveState::NotLoaded};
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
		const FObjectPathResolveResult Resolution = Durin::ResolveAssetObjectPathForOperation(
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
								ExpectedClass->GetQualifiedName())),
							.State = ESoftObjectResolveState::NotLoaded};
					if (const auto Validation = Reference.TrySetResolvedObject(
						LoadedObject, Reference.GetPath(), Reference.GetPath(),
						ExpectedClass); !Validation)
						return {
							.Result = Error(EAssetError::InvalidObjectGraph, FormatObjectError(Validation.Error)),
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
					Resolution.FinalPath.ToString(), ExpectedClass->GetQualifiedName())),
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}

		if (const auto Validation = Reference.TrySetResolvedObject(
			Object, Reference.GetPath(), Resolution.FinalPath,
			ExpectedClass); !Validation)
		{
			return {
				.Result = Error(EAssetError::InvalidObjectGraph, FormatObjectError(Validation.Error)),
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
		OutObject = nullptr;
		if (!Reference.IsNull())
			if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check(
				"soft-object load", Reference.GetPath().ToString()); !Result)
			{
				if (OutReport) *OutReport = {.RequestedPath = Reference.GetPath().GetPackagePath(),
					.PackagePath = Reference.GetPath().GetPackagePath(),
					.Error = Result.Error, .ErrorMessage = Result.Message};
				return Result;
			}
		CheckSoftObjectThread();
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

		if (const auto Validation = Reference.TrySetResolvedObject(
			LoadedObject, Reference.GetPath(), ResolvedObjectPath,
			ExpectedClass); !Validation)
			return Error(EAssetError::InvalidObjectGraph, FormatObjectError(Validation.Error));
		OutObject = LoadedObject;
		return {};
	}

}
