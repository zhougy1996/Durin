#include "DObject/GarbageCollectionScheduler.h"
#include "AssetLoadAuditInternal.h"
#include "DObject/PackagePersistence.h"
#include "PackageLoadTransaction.h"
#include "Asset/PackageUnloading.h"
#include "DObject/SoftObjectPathServices.h"
#include "Asset/RegistryOperations.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetAsyncLoadInternal.h"
#include "AssetObjectLoadTiming.h"
#include "Asset/StreamableHandle.h"
#include "AssetLiveLoadGuard.h"
#include "AssetRegistry/Scan.h"
#include "AssetMutationRegistryInternal.h"
#include "AssetRegistry/Publication.h"
#include "AssetPackageFingerprintInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "Asset/PackageResource.h"
#include "AssetPackageCodec.h"
#include "Asset/PackageVersionPolicy.h"
#include "CookedAssetRegistry.h"
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

	auto FAssetLiveLoadGuard::GetFailure() const -> FAssetReadResult
	{
		if (Failure && bEnabled && Rejections.load() != InitialRejections)
			return {EAssetReadError::InUse, "A live operation was rejected during input capture."};
		return Failure;
	}

	auto FAssetLiveLoadGuard::Check(std::string_view Operation, std::string_view Path) -> FAssetReadResult
	{
		if (ActiveCount.load() == 0) return {};
		++Rejections;
		const FAssetReadResult Result{EAssetReadError::InUse,
			std::format("Implicit live {} for '{}' is forbidden during guarded package loading.", Operation, Path)};
		for (auto* Guard = Active; Guard; Guard = Guard->Previous)
			if (Guard->Failure) Guard->Failure = Result;
		return Result;
	}
}

namespace Durin
{
	using AssetPrivate::FAssetReferenceStoreRegistry;
	using AssetPrivate::GetAssetReferenceStoreRegistry;
	using AssetPrivate::MakePackageFingerprint;

	namespace
	{
		thread_local FAssetLoadReport* GActiveAssetLoadReport = nullptr;
		thread_local FAssetLoadAudit* GActiveAssetLoadAudit = nullptr;
		thread_local uint64 GActivePackageFileReadCount = 0;
		thread_local std::vector<TWeakObjectPtr<DPackage>>* GOwnedLoadPackages = nullptr;

		auto CheckSoftObjectThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto FindPackageObject(DPackage* Package, const FSoftObjectPath& Path) -> DObject*
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

		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult
		{
			return {Code, std::move(Message)};
		}

		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult;
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			FByteView Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetReadResult;

		auto ProjectionPendingError(const FPackagePath& Path) -> FAssetReadResult
		{
			return {EAssetReadError::ProjectionPending,
				std::format("Registry projection for package {} is pending synchronization.", Path.ToString())};
		}

		auto ObjectPathResolutionError(
			const FObjectPathResolveResult& Resolution) -> FAssetReadResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::ProjectionPending:
				return ProjectionPendingError(Resolution.FinalPath.GetPackagePath());
			case EAssetPathResolveState::NotFound:
				return Error(EAssetReadError::NotFound, std::format(
					"Object {} is not present in the registry.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::MissingRedirectTarget:
				return Error(EAssetReadError::NotFound, std::format(
					"Object redirect {} has a missing target {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectCycle:
				return Error(EAssetReadError::CircularDependency, std::format(
					"Object redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectDepthExceeded:
				return Error(EAssetReadError::CircularDependency, std::format(
					"Object redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::UnknownTargetClass:
				return Error(EAssetReadError::UnknownClass, std::format(
					"Object {} resolves to a target with an unavailable reflected class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::RedirectTypeMismatch:
				return Error(EAssetReadError::TypeMismatch, std::format(
					"Object {} resolves to a target with an incompatible class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::CorruptRedirector:
				return Error(EAssetReadError::CorruptFile, std::format(
					"CorruptRedirector: object {} traverses invalid redirect metadata at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			}
			return Error(EAssetReadError::CorruptFile,
				"Object resolution returned an unknown state.");
		}

		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult
		{
			return {Code, std::move(Message)};
		}

		auto CorruptRedirector(std::string Message) -> FAssetReadResult
		{
			return Error(EAssetReadError::CorruptFile,
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

	auto FPackageLoadTransaction::LoadPackageDetailed(const FPackagePath& Path,
		FAssetLoadReport* OutReport) -> std::expected<DPackage*, FAssetReadError>
	{
		CheckSoftObjectThread();
		if (!FAssetRuntimeState::Get().GetLoadService().IsIdle())
		{
			return std::unexpected(FAssetReadError{EAssetReadError::InUse, "A load scope requires a top-level load invocation."});
		}
		struct FRestoreLoadOwner
		{
			std::vector<TWeakObjectPtr<DPackage>>* Previous;
			~FRestoreLoadOwner() { GOwnedLoadPackages = Previous; }
		} Restore{std::exchange(GOwnedLoadPackages, &Packages)};
		return Durin::LoadPackageDetailed(Path, OutReport);
	}

	auto FPackageLoadTransaction::LoadObjectDetailed(const FSoftObjectPath& Path, const DClass* ExpectedClass,
		FAssetLoadReport* OutReport) -> std::expected<DObject*, FAssetReadError>
	{
		CheckSoftObjectThread();
		if (!FAssetRuntimeState::Get().GetLoadService().IsIdle())
		{
			return std::unexpected(FAssetReadError{EAssetReadError::InUse, "A load scope requires a top-level load invocation."});
		}
		struct FRestoreLoadOwner
		{
			std::vector<TWeakObjectPtr<DPackage>>* Previous;
			~FRestoreLoadOwner() { GOwnedLoadPackages = Previous; }
		} Restore{std::exchange(GOwnedLoadPackages, &Packages)};
		return Durin::LoadObjectDetailed(Path, ExpectedClass, OutReport);
	}

	auto FPackageLoadTransaction::Unload(
		std::span<const TWeakObjectPtr<DPackage>> IgnoreSavedDependencies) -> FAssetReadResult
	{
		CheckSoftObjectThread();
		const auto Result = FAssetRuntimeState::Get().GetLoadService().ReleasePackages(
			Packages, IgnoreSavedDependencies);
		std::erase_if(Packages, [](const auto& Package) { return !Package.IsValid(); });
		return Result;
	}

	FScopedAssetLoadAudit::FScopedAssetLoadAudit(FAssetLoadAudit& Output)
		: Previous(GActiveAssetLoadAudit)
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		Output = {};
		GActiveAssetLoadAudit = &Output;
	}
	FScopedAssetLoadAudit::~FScopedAssetLoadAudit()
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		GActiveAssetLoadAudit = Previous;
	}
	auto AssetPrivate::GetActiveAssetLoadAudit() -> FAssetLoadAudit* { return GActiveAssetLoadAudit; }

	auto ReportAssetLoadMutation(DObject* Object, std::string HandlerId,
		std::string Summary, EAssetLoadMutationKind Kind) -> void
	{
		if (!Object) return;
		if (GActiveAssetLoadReport)
		{
			if (Kind == EAssetLoadMutationKind::Upgrade) ++GActiveAssetLoadReport->UpgradeMutationCount;
			else ++GActiveAssetLoadReport->NonUpgradeMutationCount;
		}
		if (!GActiveAssetLoadAudit) return;
		DPackage* Package = Object->GetPackage();
		FPackagePath PackagePath;
		if (Package) FPackagePath::TryCreate(Package->GetPackagePath(), PackagePath);
		GActiveAssetLoadAudit->Mutations.push_back({
			.PackagePath = std::move(PackagePath), .ObjectPath = Object->GetObjectPath(),
			.HandlerId = std::move(HandlerId), .Summary = std::move(Summary), .Kind = Kind});
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
		, PackageRetirement([](DObject& Object) {
			// Retirement follows actual graph teardown, not release of a user's references.
			auto* Package = Cast<DPackage>(&Object);
			if (!Package || !Package->IsGarbage() || !Package->IsAssetPackage()
				|| Package->IsGraphPrivate() || FindPackage(Package->GetPackagePath()) != Package) return;
			GetPackageResourceManager().RetirePackage(Package->GetPackagePath());
			Durin::Private::InvalidateSoftObjectCaches();
		})
	{
	}

	auto FAssetLoadService::LoadPackageDetailed(
		const FPackagePath& Path,
		FAssetLoadReport* OutReport) -> std::expected<DPackage*, FAssetReadError>
	{
		if (OutReport) *OutReport = {.RequestedPath = Path, .FinalPath = Path, .PackagePath = Path};
		if (!Path.IsValid())
			return std::unexpected(FAssetReadError{EAssetReadError::InvalidPath,
				"A package load requires a valid package path."});
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("package load", Path.ToString()); !Result)
		{
			if (OutReport) *OutReport = {.RequestedPath = Path, .PackagePath = Path};
			return std::unexpected(AssetReadErrorFromResult(Result));
		}
		if (auto Result = FAssetRuntimeState::Get().CheckRequestAdmission(); !Result)
		{
			if (OutReport) *OutReport = {.RequestedPath = Path, .PackagePath = Path};
			return std::unexpected(AssetReadErrorFromResult(Result));
		}
		if (IsAssetRegistryProjectionFenced(Path))
		{
			const auto Result = ProjectionPendingError(Path);
			if (OutReport) *OutReport = {.RequestedPath = Path, .FinalPath = Path,
				.PackagePath = Path};
			return std::unexpected(AssetReadErrorFromResult(Result));
		}
		if (IsPackageLoading(Path))
			return std::unexpected(FAssetReadError{EAssetReadError::InUse, std::format("Package '{}' is not ready for public loading.", Path.ToString())});
		if (DPackage* Resident = FindResidentPackage(Path))
		{
			if (RuntimeConfiguration.GetResidencyPolicy() == EAssetResidencyPolicy::Editor && !Resident->IsPlayInEditor())
				Resident->SetResidencyReasons(Resident->GetResidencyReasons() | EAssetPackageResidencyReason::EditorStandalone);
			return Resident;
		}
		const FPackageCatalogEntry Entry = Durin::FindPackageExact(Path);
		if (!Entry)
			return std::unexpected(FAssetReadError{EAssetReadError::NotFound, std::format(
				"Package {} is not present in the registry.", Path.ToString())});
		DPackage* Package = nullptr;
		if (auto Result = LoadPackageFromPhysicalPath(
			Path, Entry->PhysicalPath, Package, OutReport); !Result)
			return std::unexpected(AssetReadErrorFromResult(Result));
		return Package;
	}

	auto FAssetLoadService::LoadObjectDetailed(
		const FSoftObjectPath& Path,
		const DClass* ExpectedClass,
		FAssetLoadReport* OutReport) -> std::expected<DObject*, FAssetReadError>
	{
		if (OutReport) *OutReport = {
			.RequestedPath = Path.GetPackagePath(),
			.PackagePath = Path.GetPackagePath(),
			.RequestedObjectPath = Path};
		auto Finish = [&](FAssetReadResult Result, DObject* Object = nullptr) -> std::expected<DObject*, FAssetReadError>
		{
			if (OutReport)
			{
				OutReport->RequestedPath = Path.GetPackagePath();
				OutReport->RequestedObjectPath = Path;
			}
			if (!Result) return std::unexpected(AssetReadErrorFromResult(Result));
			return Object;
		};
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("object load", Path.ToString()); !Result)
			return Finish(Result);
		if (!Path.IsValid())
			return Finish(Error(EAssetReadError::InvalidPath, "An object load requires an exact object path."));
		if (auto Result = FAssetRuntimeState::Get().CheckRequestAdmission(); !Result)
			return Finish(Result);
		if (ExpectedClass && !ExpectedClass->IsChildOf(DObject::StaticClass()))
			return Finish(Error(EAssetReadError::TypeMismatch, "An object load requires a DObject class."));
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
					if (OutReport)
					{
						OutReport->FinalPath = Path.GetPackagePath();
						OutReport->ResolvedObjectPath = Path;
					}
					return Finish({}, ResidentObject);
				}
			}
			if (OutReport)
			{
				OutReport->CatalogRevision = Resolution.CatalogRevision;
				OutReport->FinalPath = Resolution.FinalPath.GetPackagePath();
				OutReport->ResolvedObjectPath = Resolution.FinalPath;
				OutReport->ObjectRedirectChain = Resolution.RedirectChain;
			}
			return Finish(ObjectPathResolutionError(Resolution));
		}
		if (OutReport)
		{
			OutReport->FinalPath = Resolution.FinalPath.GetPackagePath();
			OutReport->ResolvedObjectPath = Resolution.FinalPath;
			OutReport->CatalogRevision = Resolution.CatalogRevision;
			OutReport->ObjectRedirectChain = Resolution.RedirectChain;
		}
		auto Result = LoadPackageDetailed(
			Resolution.FinalPath.GetPackagePath(), OutReport);
		if (OutReport)
		{
			OutReport->ResolvedObjectPath = Resolution.FinalPath;
			OutReport->ObjectRedirectChain = Resolution.RedirectChain;
			OutReport->CatalogRevision = Resolution.CatalogRevision;
			OutReport->FinalPath = Resolution.FinalPath.GetPackagePath();
		}
		if (!Result) return Finish(AssetReadResultFromError(Result.error()));
		DObject* Object = FindPackageObject(*Result, Resolution.FinalPath);
		if (!Object)
			return Finish(Error(EAssetReadError::NotFound, std::format(
				"Object {} is not present in its loaded package.",
				Resolution.FinalPath.ToString())));
		if (ExpectedClass && !Object->IsA(ExpectedClass))
			return Finish(Error(EAssetReadError::TypeMismatch, std::format(
				"Object {} is not a {}.", Resolution.FinalPath.ToString(),
				ExpectedClass->GetQualifiedName())));
		if (OutReport)
		{
			OutReport->RequestedPath = Path.GetPackagePath();
			OutReport->FinalPath = Resolution.FinalPath.GetPackagePath();
			OutReport->PackagePath = Resolution.FinalPath.GetPackagePath();
			OutReport->CatalogRevision = Resolution.CatalogRevision;
			OutReport->RedirectChain.clear();
			for (const FSoftObjectPath& Redirect : Resolution.RedirectChain)
				OutReport->RedirectChain.push_back(Redirect.GetPackagePath());
			if (Resolution.FinalAssetData)
				OutReport->FinalAssetClassName =
					Resolution.FinalAssetData->AssetClassName;
		}
		return Finish({}, Object);
	}

	auto FAssetLoadService::LoadPackageFromPhysicalPath(
		const FPackagePath& Path,
		std::string_view PhysicalPath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetReadResult
	{
		AssetPrivate::FScopedObjectLoadPhase LoadPhase(Path, AssetPrivate::EObjectLoadPhase::LoadPackage);
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.Load");
		if (auto Result = FAssetRuntimeState::Get().CheckRequestAdmission(); !Result)
		{
			OutPackage = nullptr;
			return Result;
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
		FAssetReadResult Result;
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
			Result = Error(EAssetReadError::InvalidObjectGraph,
				std::format("Package '{}': load callback failed: {}", Path.ToString(), Exception.what()));
		}
		catch (...)
		{
			Result = Error(EAssetReadError::InvalidObjectGraph,
				std::format("Package '{}': load callback threw an exception.", Path.ToString()));
		}
		if (OutReport)
		{
			OutReport->PackageFileReadCount = GActivePackageFileReadCount;
		}
		return Result;
	}

	auto FAssetLoadService::ResolveDependencyPackage(FPendingLoad& Owner,
		const FPackagePath& Path, DPackage*& OutPackage) -> FAssetReadResult
	{
		auto Resolve = [&]() -> FAssetReadResult
		{
			OutPackage = nullptr;
			if (IsAssetRegistryProjectionFenced(Path)) return ProjectionPendingError(Path);
			if (const auto It = PendingLoads.find(Path); It != PendingLoads.end())
			{
				const auto& Target = *It->second;
				if (!Target.Package || Target.Phase == ELoadPhase::PostLoading || Target.Phase == ELoadPhase::Failed)
					return Error(EAssetReadError::InUse, std::format("Dependency '{}' cannot join this load component.", Path.ToString()));
				Owner.LowLink = std::min(Owner.LowLink, Target.Index);
				OutPackage = Target.Package.Get();
				return {};
			}
			if (DPackage* Resident = FindResidentPackage(Path))
			{
				OutPackage = Resident;
				return {};
			}
			const auto Entry = Durin::FindPackageExact(Path);
			if (!Entry) return Error(EAssetReadError::MissingDependency,
				std::format("Dependency '{}' is not present in the registry.", Path.ToString()));
			auto Result = LoadPackageFromPhysicalPath(Path, Entry->PhysicalPath, OutPackage);
			if (Result)
				if (const auto It = PendingLoads.find(Path); It != PendingLoads.end())
					Owner.LowLink = std::min(Owner.LowLink, It->second->LowLink);
			return Result;
		};
		auto Result = Resolve();
		if (!Result && GActiveAssetLoadReport)
			GActiveAssetLoadReport->DependencyFailures.push_back({
				Path, {}, AssetReadErrorFromResult(Result)});
		return Result;
	}

	auto FAssetLoadService::ResolveDependencyObject(FPendingLoad& Owner,
		const FSoftObjectPath& Path, DObject*& OutObject) -> FAssetReadResult
	{
		auto Resolve = [&]() -> FAssetReadResult
		{
			OutObject = nullptr;
			const auto Resolution = Durin::ResolveAssetObjectPathForOperation(Path);
			if (!Resolution)
			{
				// Match public LoadObjectDetailed's resident fallback without bypassing load phases
				// or projection fences. A catalog refresh need not evict completed objects.
				if (Resolution.State == EAssetPathResolveState::NotFound
					&& (IsPackageLoading(Path.GetPackagePath()) || FindResidentPackage(Path.GetPackagePath())))
				{
					DPackage* Resident = nullptr;
					if (auto Result = ResolveDependencyPackage(Owner, Path.GetPackagePath(), Resident); !Result)
						return Result;
					if (DObject* Object = FindPackageObject(Resident, Path))
					{
						OutObject = Object;
						return {};
					}
				}
				return ObjectPathResolutionError(Resolution);
			}
			DPackage* Package = nullptr;
			if (auto Result = ResolveDependencyPackage(Owner, Resolution.FinalPath.GetPackagePath(), Package); !Result)
				return Result;
			OutObject = FindPackageObject(Package, Resolution.FinalPath);
			return OutObject ? FAssetReadResult{} : Error(EAssetReadError::MissingDependency,
				std::format("Dependency object '{}' is absent from its package.", Resolution.FinalPath.ToString()));
		};
		auto Result = Resolve();
		if (!Result && GActiveAssetLoadReport)
			GActiveAssetLoadReport->DependencyFailures.push_back({
				Path.GetPackagePath(), Path, AssetReadErrorFromResult(Result)});
		return Result;
	}

	auto FAssetLoadService::CompleteComponent(FPendingLoad& Root) -> FAssetReadResult
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
				return Error(EAssetReadError::InvalidObjectGraph, "Load component contains unrestored values.");
			DURIN_PROFILE_CPU_ZONE_NAMED("Asset.ValidateComponent");
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
		for (const auto& Member : Group)
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Asset.PostLoadComponent");
			Member->PostLoad();
		}
		for (const auto& Member : Group)
		{
			Member->Phase = ELoadPhase::Ready;
			if (RuntimeConfiguration.GetResidencyPolicy() == EAssetResidencyPolicy::Editor && !Member->Package->IsPlayInEditor())
				Member->Package->SetResidencyReasons(Member->Package->GetResidencyReasons() | EAssetPackageResidencyReason::EditorStandalone);
			Member->bOwnResource = false;
			PendingLoads.erase(Member->Path);
			CompletionStack.pop_back();
		}
		// Bare synchronous results are observations; publication adds no cache owner.
		RequestGarbageCollection();
		return {};
	}

	auto FAssetLoadService::DiscardIncomplete(uint64 FirstIndex) noexcept -> void
	{
		// Hide every candidate first. Physical retirement can run object callbacks.
		for (const auto& Record : CompletionStack)
			if (Record->Index >= FirstIndex)
			{
				Record->Phase = ELoadPhase::Failed;
				if (DPackage* Package = Record->Package.Get())
				{
					MarkObjectHierarchyAsGarbage(Package);
					Package->RetireFailedLoadRegistration();
				}
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
		try { RequestGarbageCollection(); }
		catch (...) { /* Object retirement must not replace the original failure. */ }
	}

	auto FAssetLoadService::LoadPackageInternal(
		const FPackagePath& Path,
		std::string_view PhysicalPath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetReadResult
	{
		AssetPrivate::FScopedObjectLoadPhase LoadPhase(Path, AssetPrivate::EObjectLoadPhase::ReadPackage);
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.LoadPackage");
		auto ReadAccess = FPackageFileAccess::TryReadPackage(std::filesystem::path(PhysicalPath));
		if (!ReadAccess) return Error(EAssetReadError::InUse, "Package output is being written.");
		if (DPackage* Resident = FindResidentPackage(Path))
		{
			OutPackage = Resident;
			return {};
		}
		FAssetLoadReport LocalReport{.PackagePath = Path};
		FAssetLoadReport* CodecReport = OutReport ? OutReport : &LocalReport;
		FByteBuffer FileBytes;
		FByteView Bytes;
		uint64 PhysicalBulkBytes = 0;
		if (PhysicalPath.empty()) return Error(EAssetReadError::InvalidPath, "Asset path cannot be resolved in the selected package mode.");
		if (AsyncInputs)
		{
			const auto It = AsyncInputs->find(Path);
			if (It == AsyncInputs->end() || It->second.PhysicalPath != PhysicalPath)
				return Error(EAssetReadError::StaleData, "Async load dependency is outside the captured package closure.");
			if (!It->second.Result) return It->second.Result;
			Bytes = It->second.Bytes;
			PhysicalBulkBytes = It->second.BulkBytes;
		}
		else
		{
			auto Loaded = FFileHelper::LoadFileToArray(PhysicalPath);
			if (!Loaded) return Error(Loaded.error().NativeError == std::errc::no_such_file_or_directory
				? EAssetReadError::NotFound : EAssetReadError::IoError,
				std::format("Failed to read asset {}: {}", Path.ToString(), Loaded.error().ToString()));
			FileBytes = std::move(*Loaded);
			Bytes = FileBytes;
			std::filesystem::path BulkPath(PhysicalPath);
			BulkPath.replace_extension(".dbulk");
			std::error_code BulkError;
			if (std::filesystem::is_regular_file(BulkPath, BulkError))
				PhysicalBulkBytes = std::filesystem::file_size(BulkPath, BulkError);
			if (BulkError && BulkError != std::errc::no_such_file_or_directory)
				return Error(EAssetReadError::IoError,
					std::format("Asset {} bulk companion could not be inspected.", Path.ToString()));
		}
		++GActivePackageFileReadCount;
		const AssetPrivate::FAssetPackageReadContext HeaderContext{
			.PackageBytes = Bytes, .PackagePath = Path,
			.PhysicalPackageBytes = Bytes.size(),
			.PhysicalBulkBytes = PhysicalBulkBytes,
			.bResourceBackedBulk = true,
			.bCooked = RuntimeConfiguration.IsCooked()};
		const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
		if (auto Result = AssetPrivate::ResolveAssetPackageReader(
				Bytes, Codec); !Result)
			return Result;
		{
			FAssetPackageHeader Header;
			auto Result = [&] {
				AssetPrivate::FScopedObjectLoadPhase Phase(Path, AssetPrivate::EObjectLoadPhase::ReadHeader);
				return Codec->ReadHeader(HeaderContext, Header);
			}();
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
				.Dependencies = Header.Dependencies};
			Result = AssetPrivate::ValidateMutationPackageMetadata(
				HeaderMetadata, Header.ObjectCount, &Path);
			if (!Result) return Result;

			auto& Record = *PendingLoads.at(Path);
			if (Header.BulkSegmentExtent != 0)
			{
				ReadContext.BindBulkResource = [&](std::span<const FPackageBulkDataEntry> Entries,
					FPackageResourceHandle& OutResource) -> FAssetReadResult {
					auto Registration = GetPackageResourceManager().RegisterLoosePackage(
						Path.ToString(), std::filesystem::path(PhysicalPath),
						{Header.BulkSegmentExtent, Header.BulkSegmentDigest}, Entries);
					if (!Registration)
					{
						const EAssetReadError Code = Registration.error().Code == EPackageResourceRegistrationError::ShuttingDown
							? EAssetReadError::ShuttingDown : EAssetReadError::CorruptFile;
						return {.Error = Code, .Message = FormatPackageResourceRegistrationError(Registration.error())};
					}
					Record.bOwnResource = true;
					OutResource = std::move(*Registration);
					return {};
				};
			}
			ReadContext.DependencyLoadPolicy = AssetPrivate::FAssetPackageDependencyLoadPolicy{
				.ResolvePackage = [this, &Record](const FPackagePath& Dependency, DPackage*& Out) {
					return ResolveDependencyPackage(Record, Dependency, Out);
				},
				.ResolveObject = [this, &Record](const FSoftObjectPath& Object, DObject*& Out) {
					return ResolveDependencyObject(Record, Object, Out);
				},
			};
			ReadContext.DeferCompletion = [&Record](std::function<FAssetReadResult()> Validate, std::function<void()> Notify) {
				Record.Validate = std::move(Validate);
				Record.PostLoad = std::move(Notify);
				Record.Phase = ELoadPhase::ValuesRestored;
			};
			DPackage* Package = nullptr;
			Result = Codec->Load(
				ReadContext, Package, CodecReport,
				[&](DPackage* LoadedPackage) -> FAssetReadResult {
					if (!LoadedPackage || FindPackage(Path.GetView()) != LoadedPackage || Record.Package)
						return Error(EAssetReadError::AlreadyExists, "The package skeleton is already resident.");
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
			const FPackageCatalogEntry Data = FindPackageExact(OtherPath);
			if (!Data) continue;
			for (const FPackagePath& Dependency : Data->Dependencies)
			{
				const FPackagePathResolveResult Resolution = Durin::ResolvePackagePathForOperation(Dependency);
				if (Resolution && Resolution.FinalPath == Path) return true;
			}
		}
		return false;
	}

	auto FAssetLoadService::UnloadPackage(
		const FPackagePath& Path,
		EAssetPackageUnloadPolicy Policy) -> FAssetReadResult
	{
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("UnloadPackage", ""); !Result) return Result;
		DPackage* Package = FindResidentPackage(Path);
		if (!Package)
			return Error(EAssetReadError::NotFound, "Package is not resident.");
		if (IsPackageLoading(Path) || IsPackageReferenced(Package))
			return Error(EAssetReadError::InUse, "Package is still referenced.");
		const bool bHasUnsavedState =
			Package->IsNewlyCreated() || Package->IsDirty();
		if (bHasUnsavedState
			&& Policy == EAssetPackageUnloadPolicy::RejectUnsaved)
			return Error(EAssetReadError::InUse,
				"Package has unsaved state; explicit discard policy is required.");
		const auto Reasons = Package->GetResidencyReasons();
		Package->SetStandaloneResidency(false);
		CollectGarbage();
		if (DPackage* RemainingPackage = FindResidentPackage(Path))
		{
			RemainingPackage->SetResidencyReasons(Reasons);
			return Error(EAssetReadError::InUse,
				"Package remains referenced by live objects.");
		}
		GetPackageResourceManager().RetirePackage(Path.ToString());
		Durin::Private::InvalidateSoftObjectCaches();
		return {};
	}

	auto FAssetLoadService::ReleasePackages(
		std::span<const TWeakObjectPtr<DPackage>> Packages,
		std::span<const TWeakObjectPtr<DPackage>> IgnoreSavedDependencies) -> FAssetReadResult
	{
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("ReleasePackages", ""); !Result) return Result;
		if (LoadDepth != 0 || !PendingLoads.empty())
			return Error(EAssetReadError::InUse, "A package load is still in progress.");
		std::unordered_set<FPackagePath> Candidates;
		std::unordered_map<FPackagePath, EAssetPackageResidencyReason> OriginalReasons;
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
			OriginalReasons.emplace(Package->GetPackagePathIdentity(), Package->GetResidencyReasons());
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
				const FPackageCatalogEntry Data = FindPackageExact(Path);
				if (!Data) continue;
				for (const FPackagePath& Dependency : Data->Dependencies)
				{
					const FPackagePathResolveResult Resolution = Durin::ResolvePackagePathForOperation(Dependency);
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
				Remaining->SetResidencyReasons(OriginalReasons.at(Path));
				bInUse = true;
			}
			else
			{
				GetPackageResourceManager().RetirePackage(Path.ToString());
				bReleased = true;
			}
		}
		if (bReleased) Durin::Private::InvalidateSoftObjectCaches();
		return bInUse ? Error(EAssetReadError::InUse,
			"Packages remain referenced or have unsaved state.") : FAssetReadResult{};
	}

	auto FAssetRuntimeState::Shutdown() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!AssetPrivate::FAssetLiveLoadGuard::Check("Shutdown", "")) return;
		if (State == EState::Uninitialized || State == EState::Stopped) return;
		StopAcceptingRequests();
		CancelAsyncLoading();
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
		State = EState::Stopped;
		Durin::Private::InvalidateSoftObjectCaches();
	}

	auto FAssetRuntimeState::Initialize(FAssetRuntimeConfiguration Configuration)
		-> FAssetWriteResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("Initialize", ""); !Result) return AssetWriteResultFromRead(Result);
		if (State == EState::Running)
		{
			if (RuntimeConfiguration == Configuration) return {};
			return Error(EAssetWriteError::InUse,
				"Asset runtime configuration cannot be replaced while Engine Asset is initialized.");
		}
		if (State == EState::Stopping)
			return Error(EAssetWriteError::InUse, "Asset shutdown must complete before initialization.");
		// First initialization must not touch reflected types before DObjectInit.
		if (!Loader.IsIdle() || (State == EState::Stopped && !GetResidentAssetPackages().empty()))
			return Error(EAssetWriteError::InUse, "Asset loading or resident package cleanup is incomplete.");
		if (Configuration.IsCooked())
			if (auto Result = AssetPrivate::InitializeCookedAssetRegistry(Configuration); !Result) return AssetWriteResultFromRead(Result);
		RuntimeConfiguration = std::move(Configuration);
		PackageSavePrivate::SetAsyncSaveAdmission(true);
		bAcceptingRequests = true;
		State = EState::Running;
		return {};
	}

	auto FAssetRuntimeState::CheckRequestAdmission() const -> FAssetReadResult
	{
		if (State == EState::Uninitialized)
			return Error(EAssetReadError::InUse, "Asset loading requires InitializeAssetRuntime before use.");
		if (!bAcceptingRequests)
			return Error(EAssetReadError::ShuttingDown, "Asset requests are closed for shutdown.");
		return {};
	}

	auto FAssetRuntimeState::StopAcceptingRequests() -> void
	{
		if (!AssetPrivate::FAssetLiveLoadGuard::Check("StopAcceptingRequests", "")) return;
		if (!bAcceptingRequests) return;
		bAcceptingRequests = false;
		State = EState::Stopping;
		DURIN_DEBUG("Asset manager stopped accepting new requests");
	}

	auto FAssetLoadService::FindLoadedObject(const FSoftObjectPath& Path,
		const DClass* ExpectedClass, DObject* Cached) -> DObject*
	{
		if (!AssetPrivate::FAssetLiveLoadGuard::Check("soft-object lookup", Path.ToString())) return nullptr;
		CheckSoftObjectThread();
		if (!bAcceptingRequests) return nullptr;
		// Catalog, fence, and object-identity changes invalidate the private cache epoch.
		if (Cached) return Cached;
		const auto Resolution = ResolveAssetObjectPathForOperation(Path, {.ExpectedClass = ExpectedClass});
		if (Resolution)
		{
			DPackage* Package = FindResidentPackage(Resolution.FinalPath.GetPackagePath());
			return Package && !IsPackageLoading(Resolution.FinalPath.GetPackagePath())
				? FindPackageObject(Package, Resolution.FinalPath) : nullptr;
		}
		if (Resolution.State != EAssetPathResolveState::NotFound || IsPackageLoading(Path.GetPackagePath())) return nullptr;
		DObject* Object = FindPackageObject(FindResidentPackage(Path.GetPackagePath()), Path);
		return Object && !Object->IsA<DAssetRedirector>() ? Object : nullptr;
	}
}
