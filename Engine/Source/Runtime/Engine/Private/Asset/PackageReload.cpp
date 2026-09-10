#include "Asset/PackageReload.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "Asset/PackageResource.h"
#include "Asset/RegistryOperations.h"
#include "AssetPackageLinker.h"
#include "Components/PrimitiveComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/SkyLightComponent.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/MountPaths.h"
#include "DynamicRHI.h"
#include "Texture/Texture2D.h"
#include "Texture/VolumeTexture.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		using Status = EPackageReloadStatus;
		using Failure = EPackageReloadFailure;
		using Stage = EPackageReloadStage;
		bool GPackageReloadActive = false;

		auto MakeResult(Status InStatus, Failure InFailure, Stage InStage,
			const FPackagePath& Path, std::string Message) -> FPackageReloadResult
		{
			FPackageReloadResult Result{.Status = InStatus, .Failure = InFailure};
			if (!Message.empty()) Result.Diagnostics.push_back({Path, {}, InStage, std::move(Message)});
			return Result;
		}

		auto IsCancelled(const FPackageReloadRequest& Request) -> bool
		{
			return Request.IsCancelled && Request.IsCancelled();
		}

		auto Injected(const FPackageReloadRequest& Request,
			EPackageReloadFaultPoint Point, uint64 Package = 0, uint64 Object = 0) -> bool
		{
			return Request.ShouldFail && Request.ShouldFail(Point, Package, Object);
		}

		auto ResolveAuthoredPath(const FPackagePath& Path) -> std::filesystem::path
		{
			const FAssetPathResult Resolved = FMountPaths::ResolveAssetPath(
				Path.GetView(), EMountPathExistence::AllowMissing);
			return Resolved ? std::filesystem::path(Resolved.PhysicalPath.generic_string() + ".dasset")
				: std::filesystem::path{};
		}

		auto GatherHierarchy(DObject* Root, EObjectQueryScope Scope,
			std::vector<DObject*>& Out) -> void
		{
			if (!Root) return;
			Out.push_back(Root);
			for (DObject* Child : GDObjectArray.GetObjectsWithOuter(Root, Scope))
				GatherHierarchy(Child, Scope, Out);
		}

		auto IsSupportedTopLevel(const DObject& Object) -> bool
		{
			const DClass* Class = Object.GetClass();
			return Class == DTexture2D::StaticClass()
				|| Class == DVolumeTexture::StaticClass()
				|| Class == DMaterial::StaticClass()
				|| Class == DMaterialInstance::StaticClass();
		}

		auto MakeAdmittedClasses() -> std::vector<const DClass*>
		{
			std::vector<const DClass*> Result{
				DTexture2D::StaticClass(), DVolumeTexture::StaticClass(),
				DMaterial::StaticClass(), DMaterialInstance::StaticClass()};
			// Inner asset objects (for example import provenance) are admitted by
			// exact registered class after the top-level family has been checked.
			for (DClass* Class : GetDerivedClasses(DObject::StaticClass(), true))
				if (Class && std::ranges::find(Result, Class) == Result.end())
					Result.push_back(Class);
			return Result;
		}

		auto MapGraphFailure(AssetPrivate::EPackageGraphPrepareStatus Value) -> Failure
		{
			using S = AssetPrivate::EPackageGraphPrepareStatus;
			switch (Value)
			{
			case S::Unsupported: return Failure::Unsupported;
			case S::BudgetExceeded: return Failure::BudgetExceeded;
			case S::Stale: return Failure::Stale;
			case S::Busy: return Failure::Busy;
			case S::MissingDependency: return Failure::InvalidClosure;
			case S::InvalidClosure: return Failure::InvalidClosure;
			case S::Cancelled: return Failure::None;
			case S::ValuesPrepared: return Failure::None;
			}
			return Failure::InvalidClosure;
		}

		auto MapReplacementFailure(EObjectReplacementError Value) -> Failure
		{
			switch (Value)
			{
			case EObjectReplacementError::IncompatibleType: return Failure::IncompatibleGraph;
			case EObjectReplacementError::UnmappedReference: return Failure::UnmappedReference;
			case EObjectReplacementError::BudgetExceeded:
			case EObjectReplacementError::AllocationFailure: return Failure::BudgetExceeded;
			case EObjectReplacementError::Stale: return Failure::Stale;
			case EObjectReplacementError::Busy: return Failure::Busy;
			case EObjectReplacementError::ParticipantRejected: return Failure::ParticipantRejected;
			case EObjectReplacementError::Unsupported: return Failure::Unsupported;
			case EObjectReplacementError::InvalidGraph:
			case EObjectReplacementError::MapCollision: return Failure::IncompatibleGraph;
			case EObjectReplacementError::None: return Failure::None;
			}
			return Failure::IncompatibleGraph;
		}

		auto AddBytes(uint64 Value, uint64 Limit, uint64& Total) -> bool
		{
			if (Value > Limit - Total) return false;
			Total += Value;
			return true;
		}

		auto PrepareRuntimeProducts(std::span<AssetPrivate::FPreparedPackageGraph> Graphs,
			const FPackageReloadBudget& Budget, uint64 ClosureBytes,
			FPackageReloadResult& OutResult) -> bool
		{
			uint64 CpuBytes = ClosureBytes;
			uint64 GpuBytes = 0;
			for (auto& Graph : Graphs)
			{
				std::vector<DObject*> Objects;
				GatherHierarchy(Graph.GetPackage(), EObjectQueryScope::IncludeUnpublished, Objects);
				for (size_t Reverse = Objects.size(); Reverse > 1; --Reverse)
				{
					DObject* Object = Objects[Reverse - 1];
					Object->PostLoad();
					Object->ClearLoadedCustomVersions();
					Object->ClearLoadedDeprecatedProperties();
				}
				FAssetCompilingManager::Get().FinishCompilationForObjects(Objects);
				for (DObject* Object : Objects)
				{
					if (auto* Texture = Cast<DTexture>(Object);
						Texture && (!Texture->HasPlatformData()
							|| !Texture->FinishReloadResourcePreparation()))
					{
						OutResult = MakeResult(Status::Failed, Failure::ResourcePreparationFailed,
							Stage::PrepareRuntimeProducts, {},
							std::format("Runtime product preparation failed for {}.", Object->GetObjectPath()));
						return false;
					}
					if (auto* Material = Cast<DMaterialInterface>(Object);
						GDynamicRHI && Material
						&& Material->GetMaterialCompileStatus().State == EMaterialCompileState::Failed)
					{
						OutResult = MakeResult(Status::Failed, Failure::ResourcePreparationFailed,
							Stage::PrepareRuntimeProducts, {},
							std::format("Material preparation failed for {}.", Object->GetObjectPath()));
						return false;
					}
					if (auto* Texture2D = Cast<DTexture2D>(Object))
					{
						for (const auto& Mip : Texture2D->GetPlatformData()->Mips)
							if (!AddBytes(Mip.Pixels.size(), Budget.MaximumRetainedCpuBytes, CpuBytes)
								|| !AddBytes(Mip.Pixels.size(), Budget.MaximumCandidateGpuBytes, GpuBytes))
							{
								OutResult = MakeResult(Status::Failed, Failure::BudgetExceeded,
									Stage::PrepareRuntimeProducts, {},
									"Texture runtime products exceed the reload memory budget.");
								return false;
							}
					}
					else if (auto* Volume = Cast<DVolumeTexture>(Object))
					{
						for (const auto& Mip : Volume->GetPlatformData()->Mips)
							if (!AddBytes(Mip.Voxels.size(), Budget.MaximumRetainedCpuBytes, CpuBytes)
								|| !AddBytes(Mip.Voxels.size(), Budget.MaximumCandidateGpuBytes, GpuBytes))
							{
								OutResult = MakeResult(Status::Failed, Failure::BudgetExceeded,
									Stage::PrepareRuntimeProducts, {},
									"Volume runtime products exceed the reload memory budget.");
								return false;
							}
					}
				}
			}
			return true;
		}

		auto RefreshExternalRenderBindings(std::span<DObject* const> Objects,
			bool bTextures, bool bMaterials) -> void
		{
			for (DObject* Object : Objects)
			{
				if (bTextures)
				{
					if (auto* Cloud = Cast<DVolumetricCloudComponent>(Object))
						Cloud->RefreshReloadedAssetBindings();
					if (auto* SkyLight = Cast<DSkyLightComponent>(Object))
						SkyLight->RefreshReloadedAssetBindings();
					if (auto* Material = Cast<DMaterialInterface>(Object))
						Material->RefreshReloadedAssetBindings();
				}
				if (bMaterials)
					if (auto* Primitive = Cast<DPrimitiveComponent>(Object))
						Primitive->MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
			}
		}
	}

	struct FPackageReloadOperation::FState
	{
		FPackageReloadResult Result;
		std::unique_ptr<FObjectGraphReplacement> Replacement;
		std::atomic_bool bCancelRequested = false;
	};

	auto FPackageReloadResourceReceipt::GetState() const -> EPackageReloadReceiptState
	{
		std::lock_guard Lock(Mutex); return State;
	}
	auto FPackageReloadResourceReceipt::SetReady() -> bool
	{
		std::lock_guard Lock(Mutex);
		if (State != EPackageReloadReceiptState::Pending) return false;
		State = EPackageReloadReceiptState::Ready; return true;
	}
	auto FPackageReloadResourceReceipt::SetFailed(std::string InMessage) -> bool
	{
		std::lock_guard Lock(Mutex);
		if (State != EPackageReloadReceiptState::Pending) return false;
		State = EPackageReloadReceiptState::Failed; Message = std::move(InMessage); return true;
	}
	auto FPackageReloadResourceReceipt::SetRetired() -> bool
	{
		std::lock_guard Lock(Mutex);
		if (State != EPackageReloadReceiptState::Ready) return false;
		State = EPackageReloadReceiptState::Retired; return true;
	}
	auto FPackageReloadResourceReceipt::GetMessage() const -> std::string
	{
		std::lock_guard Lock(Mutex); return Message;
	}

	FPackageReloadOperation::FPackageReloadOperation()
		: State(std::make_shared<FState>()) {}
	FPackageReloadOperation::~FPackageReloadOperation() = default;
	FPackageReloadOperation::FPackageReloadOperation(FPackageReloadOperation&&) noexcept = default;
	auto FPackageReloadOperation::operator=(FPackageReloadOperation&&) noexcept
		-> FPackageReloadOperation& = default;
	auto FPackageReloadOperation::Poll() -> FPackageReloadResult
	{
		if (!State) return MakeResult(Status::Failed, Failure::Busy, Stage::Retire, {}, "Reload operation has no state.");
		if (State->Result.Status == Status::Pending && State->Replacement
			&& State->Replacement->Retire())
		{
			State->Replacement.reset();
			State->Result.Status = Status::Succeeded;
			GPackageReloadActive = false;
		}
		return State->Result;
	}
	auto FPackageReloadOperation::Wait() -> FPackageReloadResult
	{
		// Retirement is deliberately non-blocking with respect to arbitrary
		// consumers; synchronous callers receive Pending and may poll next frame.
		return Poll();
	}
	auto FPackageReloadOperation::Cancel() -> void
	{
		if (State) State->bCancelRequested = true;
	}
	auto FPackageReloadOperation::GetResult() const -> FPackageReloadResult
	{
		return State ? State->Result
			: MakeResult(Status::Failed, Failure::Busy, Stage::Retire, {}, "Reload operation has no state.");
	}

	auto ReloadPackages(const FPackageReloadRequest& Request) -> FPackageReloadOperation
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		auto State = std::make_shared<FPackageReloadOperation::FState>();
		FPackageReloadOperation Operation(State);
		if (GPackageReloadActive)
		{
			State->Result = MakeResult(Status::Failed, Failure::Busy, Stage::Preflight, {},
				"Another package reload is active.");
			return Operation;
		}
		GPackageReloadActive = true;
		auto Finish = [&](FPackageReloadResult Result) {
			State->Result = std::move(Result);
			if (State->Result.Status != Status::Pending) GPackageReloadActive = false;
			return std::move(Operation);
		};
		auto Cancelled = [&] { return State->bCancelRequested || IsCancelled(Request); };
		if (Cancelled()) return Finish(MakeResult(Status::Cancelled, Failure::None,
			Stage::Preflight, {}, "Package reload was cancelled."));
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
			return Finish(MakeResult(Status::Failed, Failure::Unsupported, Stage::Preflight, {},
				"Authored package reload is unavailable in a cooked runtime."));
		if (Request.Packages.empty()) return Finish(MakeResult(Status::Failed,
			Failure::Unsupported, Stage::Preflight, {}, "Package reload requires at least one package."));

		std::vector<DPackage*> Packages;
		Packages.reserve(Request.Packages.size());
		for (DPackage* Package : Request.Packages)
		{
			if (!IsValid(Package) || !Package->IsAssetPackage() || Package->IsGraphPrivate())
				return Finish(MakeResult(Status::Failed, Failure::Unsupported, Stage::Preflight, {},
					"Reload accepts only resident authored asset packages."));
			if (std::ranges::find(Packages, Package) == Packages.end()) Packages.push_back(Package);
		}
		if (Injected(Request, EPackageReloadFaultPoint::PreflightBudget)
			|| Packages.size() > Request.Budget.MaximumPackages)
			return Finish(MakeResult(Status::Failed, Failure::BudgetExceeded, Stage::Preflight, {},
				"Package reload count exceeds the request budget."));

		std::vector<FPackagePath> Paths;
		std::vector<std::filesystem::path> Files;
		std::vector<DObject*> OldObjects;
		std::vector<DObject*> ExternalRenderConsumers =
			GDObjectArray.GetAll(EObjectQueryScope::LiveOnly);
		bool bTextures = false, bMaterials = false;
		for (DPackage* Package : Packages)
		{
			FPackagePath Path;
			if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path))
				return Finish(MakeResult(Status::Failed, Failure::Unsupported, Stage::Preflight, {},
					"Resident package identity is invalid."));
			if (Package->IsNewlyCreated())
				return Finish(MakeResult(Status::Failed, Failure::Unsaved, Stage::Preflight, Path,
					"A never-saved package has no disk baseline."));
			const std::filesystem::path File = ResolveAuthoredPath(Path);
			if (File.empty() || !std::filesystem::exists(File))
				return Finish(MakeResult(Status::Failed, Failure::IoError, Stage::Preflight, Path,
					"The saved package file is missing."));
			std::vector<DObject*> Objects;
			GatherHierarchy(Package, EObjectQueryScope::LiveOnly, Objects);
			for (DObject* Object : Objects)
			{
				if (Object->GetOuter() == Package && !IsSupportedTopLevel(*Object))
					return Finish(MakeResult(Status::Failed, Failure::Unsupported, Stage::Preflight, Path,
						std::format("Asset class {} has no reload participant.", Object->GetClass()->GetName())));
				bTextures |= Object->IsA(DTexture::StaticClass());
				bMaterials |= Object->IsA(DMaterialInterface::StaticClass());
			}
			OldObjects.insert(OldObjects.end(), Objects.begin(), Objects.end());
			Paths.push_back(Path); Files.push_back(File);
		}
		std::erase_if(ExternalRenderConsumers, [&](DObject* Object) {
			return std::ranges::find(OldObjects, Object) != OldObjects.end();
		});

		if (Injected(Request, EPackageReloadFaultPoint::QuiesceSelected))
			return Finish(MakeResult(Status::Failed, Failure::Busy, Stage::Quiesce, {},
				"Injected selected-compilation quiesce failure."));
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(OldObjects);
		FAssetCompilingManager::Get().FinishCompilationForObjects(OldObjects);
		if (Cancelled()) return Finish(MakeResult(Status::Cancelled, Failure::None,
			Stage::Quiesce, {}, "Package reload was cancelled."));

		try
		{
			std::vector<AssetPrivate::FPackageGraphSource> Sources(Paths.size());
			uint64 RemainingBytes = Request.Budget.MaximumRetainedCpuBytes;
			for (size_t Index = 0; Index < Paths.size(); ++Index)
			{
				if (Injected(Request, EPackageReloadFaultPoint::ReadMain, Index))
					return Finish(MakeResult(Status::Failed, Failure::IoError,
						Stage::ReadAndPrepare, Paths[Index], "Injected main package read failure."));
				auto Read = FPreparedPackageResource::Read(
					Paths[Index], Files[Index], RemainingBytes, Sources[Index].Storage, Cancelled);
				if (!Read)
				{
					const bool bCancelled = Read.Status == EPreparedPackageResourceStatus::Cancelled;
					const Failure Code = Read.Status == EPreparedPackageResourceStatus::BudgetExceeded
						? Failure::BudgetExceeded : Read.Status == EPreparedPackageResourceStatus::InvalidClosure
						? Failure::InvalidClosure : Read.Status == EPreparedPackageResourceStatus::Stale
						? Failure::Stale : Failure::IoError;
					return Finish(MakeResult(bCancelled ? Status::Cancelled : Status::Failed,
						bCancelled ? Failure::None : Code, Stage::ReadAndPrepare, Paths[Index], Read.Message));
				}
				Sources[Index].PackagePath = Paths[Index];
				RemainingBytes -= Sources[Index].Storage.GetRetainedBytes();
				if (Injected(Request, EPackageReloadFaultPoint::ReadBulk, Index))
					return Finish(MakeResult(Status::Failed, Failure::IoError,
						Stage::ReadAndPrepare, Paths[Index], "Injected bulk closure read failure."));
			}

			FAssetPackageLoadScope DependencyScope;
			std::vector<AssetPrivate::FPreparedPackageGraph> Graphs;
			AssetPrivate::FPackageGraphPrepareOptions Options;
			Options.AdmittedClasses = MakeAdmittedClasses();
			Options.MaximumPackages = Request.Budget.MaximumPackages;
			Options.MaximumObjects = Request.Budget.MaximumObjects;
			Options.MaximumRetainedBytes = Request.Budget.MaximumRetainedCpuBytes;
			Options.DependencyLoadScope = &DependencyScope;
			Options.IsCancelled = Cancelled;
			Options.ShouldFail = [&](uint64 Package, AssetPrivate::ELinkerLoadPhase Phase, uint64 Object) {
				using L = AssetPrivate::ELinkerLoadPhase;
				EPackageReloadFaultPoint Point = EPackageReloadFaultPoint::ApplyValues;
				switch (Phase)
				{
				case L::CreateSkeleton: Point = EPackageReloadFaultPoint::CreateSkeleton; break;
				case L::ResolveDependency: Point = EPackageReloadFaultPoint::ResolveDependency; break;
				case L::ApplyValues: Point = EPackageReloadFaultPoint::ApplyValues; break;
				case L::RestoreLedger: Point = EPackageReloadFaultPoint::RestoreLedger; break;
				case L::PostLoad: Point = EPackageReloadFaultPoint::PreparePostLoad; break;
				case L::Publish: Point = EPackageReloadFaultPoint::BeforeCommit; break;
				}
				return Injected(Request, Point, Package, Object);
			};
			const auto Prepared = AssetPrivate::PreparePackageGraphs(Sources, Options, Graphs);
			if (!Prepared)
			{
				Graphs.clear();
				std::vector<TWeakObjectPtr<DPackage>> Ignore;
				for (DPackage* Package : Packages) Ignore.emplace_back(Package);
				(void)DependencyScope.Release(Ignore);
				const bool bCancelled = Prepared.Status == AssetPrivate::EPackageGraphPrepareStatus::Cancelled;
				return Finish(MakeResult(bCancelled ? Status::Cancelled : Status::Failed,
					bCancelled ? Failure::None : MapGraphFailure(Prepared.Status),
					Stage::ReadAndPrepare, Prepared.PackagePath, Prepared.Message));
			}

			uint64 ClosureBytes = 0;
			for (const auto& Source : Sources) ClosureBytes += Source.Storage.GetRetainedBytes();
			FPackageReloadResult RuntimeResult;
			if (Injected(Request, EPackageReloadFaultPoint::PreparePostLoad)
				|| Injected(Request, EPackageReloadFaultPoint::PrepareRuntimeProduct)
				|| !PrepareRuntimeProducts(Graphs, Request.Budget, ClosureBytes, RuntimeResult))
			{
				Graphs.clear();
				std::vector<TWeakObjectPtr<DPackage>> Ignore;
				for (DPackage* Package : Packages) Ignore.emplace_back(Package);
				(void)DependencyScope.Release(Ignore);
				if (RuntimeResult.Status == Status::Pending)
					RuntimeResult = MakeResult(Status::Failed, Failure::ResourcePreparationFailed,
						Stage::PrepareRuntimeProducts, {}, "Injected runtime product failure.");
				return Finish(std::move(RuntimeResult));
			}

			std::vector<FObjectReplacementPackagePair> Pairs;
			for (size_t Index = 0; Index < Packages.size(); ++Index)
				Pairs.push_back({Packages[Index], Graphs[Index].GetPackage()});
			std::vector<std::shared_ptr<IObjectReplacementParticipant>> Participants(
				Request.Participants.begin(), Request.Participants.end());
			if (Injected(Request, EPackageReloadFaultPoint::PrepareReferences)
				|| Injected(Request, EPackageReloadFaultPoint::PrepareNativeParticipant)
				|| Injected(Request, EPackageReloadFaultPoint::PrepareHistory)
				|| Injected(Request, EPackageReloadFaultPoint::ReserveRenderPublish))
			{
				Graphs.clear();
				std::vector<TWeakObjectPtr<DPackage>> Ignore;
				for (DPackage* Package : Packages) Ignore.emplace_back(Package);
				(void)DependencyScope.Release(Ignore);
				return Finish(MakeResult(Status::Failed, Failure::ParticipantRejected,
					Stage::PrepareReferences, {}, "Injected reference preparation failure."));
			}
			auto Replacement = std::make_unique<FObjectGraphReplacement>();
			const FObjectReplacementBudget ReplacementBudget{
				Request.Budget.MaximumPackages, Request.Budget.MaximumObjects,
				Request.Budget.MaximumReferenceSlots};
			auto ReplaceResult = Replacement->Prepare(Pairs, Participants, ReplacementBudget);
			if (!ReplaceResult)
			{
				Graphs.clear();
				std::vector<TWeakObjectPtr<DPackage>> Ignore;
				for (DPackage* Package : Packages) Ignore.emplace_back(Package);
				(void)DependencyScope.Release(Ignore);
				return Finish(MakeResult(Status::Failed, MapReplacementFailure(ReplaceResult.Error),
					Stage::PrepareReferences, {}, ReplaceResult.Message));
			}
			for (size_t SourceIndex = 0; SourceIndex < Sources.size(); ++SourceIndex)
			{
				const auto& Source = Sources[SourceIndex];
				if (Injected(Request, EPackageReloadFaultPoint::RevalidateDisk, SourceIndex))
				{
					Replacement->Abort(); Graphs.clear();
					std::vector<TWeakObjectPtr<DPackage>> Ignore;
					for (DPackage* Package : Packages) Ignore.emplace_back(Package);
					(void)DependencyScope.Release(Ignore);
					return Finish(MakeResult(Status::Failed, Failure::Stale,
						Stage::Revalidate, Paths[SourceIndex], "Injected disk revalidation failure."));
				}
				if (auto Revalidate = Source.Storage.Revalidate(Cancelled); !Revalidate)
				{
					Replacement->Abort(); Graphs.clear();
					std::vector<TWeakObjectPtr<DPackage>> Ignore;
					for (DPackage* Package : Packages) Ignore.emplace_back(Package);
					(void)DependencyScope.Release(Ignore);
					const bool bCancelled = Revalidate.Status == EPreparedPackageResourceStatus::Cancelled;
					return Finish(MakeResult(bCancelled ? Status::Cancelled : Status::Failed,
						bCancelled ? Failure::None : Failure::Stale, Stage::Revalidate, {}, Revalidate.Message));
				}
			}
			if (Injected(Request, EPackageReloadFaultPoint::RevalidateReferencers)
				|| Injected(Request, EPackageReloadFaultPoint::BeforeCommit))
			{
				Replacement->Abort(); Graphs.clear();
				std::vector<TWeakObjectPtr<DPackage>> Ignore;
				for (DPackage* Package : Packages) Ignore.emplace_back(Package);
				(void)DependencyScope.Release(Ignore);
				return Finish(MakeResult(Status::Failed, Failure::Stale,
					Stage::Revalidate, {}, "Injected final reload validation failure."));
			}
			ReplaceResult = Replacement->TryCommit();
			if (!ReplaceResult)
			{
				Replacement->Abort(); Graphs.clear();
				std::vector<TWeakObjectPtr<DPackage>> Ignore;
				for (DPackage* Package : Packages) Ignore.emplace_back(Package);
				(void)DependencyScope.Release(Ignore);
				return Finish(MakeResult(Status::Failed, MapReplacementFailure(ReplaceResult.Error),
					Stage::Commit, {}, ReplaceResult.Message));
			}

			State->Result = {.Status = Status::Pending};
			State->Replacement = std::move(Replacement);
			try
			{
				Graphs.clear();
				std::vector<TWeakObjectPtr<DPackage>> Ignore;
				Ignore.reserve(Packages.size());
				for (DPackage* Package : Packages) Ignore.emplace_back(Package);
				const FAssetResult ReleaseResult = DependencyScope.Release(Ignore);
				if (!ReleaseResult)
				{
					// Publication is already committed. Dependency retention is safe and
					// cannot be represented as an ordinary rollback failure.
					State->Result.Diagnostics.push_back({{}, {}, Stage::Retire, ReleaseResult.Message});
				}
				RefreshExternalRenderBindings(ExternalRenderConsumers, bTextures, bMaterials);
			}
			catch (...)
			{
				// Commit cannot be rolled back. Keep the operation successful/pending
				// and retain dependencies if a best-effort observer refresh throws.
				State->Result.Diagnostics.push_back({{}, {}, Stage::Publish,
					"A post-publication refresh threw; the replacement remains committed."});
			}
			return Operation;
		}
		catch (const std::bad_alloc&)
		{
			return Finish(MakeResult(Status::Failed, Failure::BudgetExceeded,
				Stage::ReadAndPrepare, {}, "Allocation failed while preparing package reload."));
		}
		catch (...)
		{
			return Finish(MakeResult(Status::Failed, Failure::InvalidClosure,
				Stage::ReadAndPrepare, {}, "A package reload callback threw."));
		}
	}
}
