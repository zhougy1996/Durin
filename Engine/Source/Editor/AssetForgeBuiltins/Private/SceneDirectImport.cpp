#include "Misc/FeedbackContext.h"
#include "DObject/StrongObjectPtr.h"
#include "Asset/Testing/PackageSaveTesting.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsMeshCompilation.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "StaticMesh/StaticMeshImportSettings.h"
#include "AssetForge/Builtins/AssetImport.h"
#include "AssetTools/AssetImportAdapter.h"
#include "AssetImportTask.h"
#include "Import/AssetImportTaskOperation.h"
#include "AssetForge/Builtins/SceneImportOptions.h"
#include "DObject/WeakObjectPtr.h"
#include "AssetForge/Builtins/SceneImportData.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Threading/Task.h"
#include <coroutine>

#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/AssetImportData.h"
#include "Asset/PackageSerialization.h"
#include "Asset/SourceHint.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialInstance.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "SceneImportInternal.h"
#include "Import/AssetDestinationValidation.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMeshImportAdapter.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DCompilation.h"

namespace Durin::AssetForge::Builtins
{
namespace
{
auto ResolveImportedMaterialSlot(const DStaticMesh &Mesh, uint32 SourceMaterialIndex, uint32 &OutSlotIndex,
                                 std::string &OutError) -> bool
{
	const auto Slots = Mesh.GetMaterialSlots();
	const auto Slot =
	    std::ranges::find(Slots, SourceMaterialIndex, &FMeshMaterialSlotDefinition::SourceMaterialIndex);
	if (Slot == Slots.end())
	{
		OutError = std::format("Static mesh has no slot for source material {}.", SourceMaterialIndex);
		return false;
	}
	if (std::find_if(std::next(Slot), Slots.end(), [&](const auto &Other)
	                 { return Other.SourceMaterialIndex == SourceMaterialIndex; }) != Slots.end())
	{
		OutError =
		    std::format("Static mesh has ambiguous slots for source material {}.", SourceMaterialIndex);
		return false;
	}
	OutSlotIndex = static_cast<uint32>(std::distance(Slots.begin(), Slot));
	OutError.clear();
	return true;
}

struct FPreparedSceneOutput
{
	const FSceneOutputData *Descriptor = nullptr;
	FPackagePath AssetPath;
	FStaticMeshSource StaticMeshSource;
	std::vector<FMeshMaterialSlotDefinition> StaticMeshMaterialSlots;
	std::unique_ptr<FStaticMeshRenderData> StaticMesh;
	FSceneTextureBuildProduct Texture;
	DObject *Candidate = nullptr;
	DPackage *Package = nullptr;
	TStrongObjectPtr<DObject> CandidatePin;
};

auto AddError(FAssetImportResult &Result, EImportDiagnosticCategory Category, std::string Phase,
              std::string Message, std::string OutputIdentity = {}) -> FAssetImportResult
{
	if (!OutputIdentity.empty())
		if (auto Output = std::ranges::find(Result.Outputs, OutputIdentity, &FImportOutputSummary::StableIdentity);
			Output != Result.Outputs.end()) Output->State = EImportOutputState::Failed;
	Result.Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
	                              .Category = Category,
	                              .Phase = std::move(Phase),
	                              .OutputIdentity = std::move(OutputIdentity),
	                              .Message = Message});
	Result.Message = std::move(Message);
	return std::move(Result);
}

auto IsCanceled(const std::function<bool()> &Predicate) -> bool
{
	return Predicate && Predicate();
}

auto MakeStableOutputOrder(const FAssetImportPlan &Data, std::vector<size_t> &OutOrder, std::string &OutError)
    -> bool
{
	std::unordered_map<std::string, size_t> Indices;
	for (size_t Index = 0; Index < Data.Outputs.size(); ++Index)
		if (!Indices.emplace(Data.Outputs[Index].StableIdentity, Index).second)
		{
			OutError = "Scene output identities are not unique.";
			return false;
		}
	std::vector<std::vector<size_t>> Dependents(Data.Outputs.size());
	std::vector<size_t> RemainingDependencies(Data.Outputs.size(), 0);
	for (size_t Index = 0; Index < Data.Outputs.size(); ++Index)
	{
		const FSceneOutputData &Output = Data.Outputs[Index];
		std::vector<std::string> Dependencies = Output.Dependencies;
		std::ranges::sort(Dependencies);
		Dependencies.erase(std::unique(Dependencies.begin(), Dependencies.end()), Dependencies.end());
		for (const std::string &Identity : Dependencies)
		{
			const auto Found = Indices.find(Identity);
			if (Found == Indices.end())
			{
				OutError = std::format("Scene output '{}' depends on missing output '{}'.",
				                       Output.StableIdentity, Identity);
				return false;
			}
			++RemainingDependencies[Index];
			Dependents[Found->second].push_back(Index);
		}
	}
	OutOrder.clear();
	OutOrder.reserve(Data.Outputs.size());
	while (OutOrder.size() != Data.Outputs.size())
	{
		const auto Ready = std::ranges::find(RemainingDependencies, 0u);
		if (Ready == RemainingDependencies.end())
		{
			OutError = "Scene output dependencies contain a cycle.";
			return false;
		}
		const size_t Index = static_cast<size_t>(Ready - RemainingDependencies.begin());
		RemainingDependencies[Index] = std::numeric_limits<size_t>::max();
		OutOrder.push_back(Index);
		for (const size_t Dependent : Dependents[Index])
			--RemainingDependencies[Dependent];
	}
	OutError.clear();
	return true;
}

auto Abandon(std::vector<FPreparedSceneOutput> &Outputs) -> void
{
	std::vector<DObject *> Objects;
	for (const auto &Output : Outputs)
		if (Output.Candidate && Output.Package->IsGraphPrivate())
			Objects.push_back(Output.Candidate);
	FAssetCompilingManager::Get().MarkCompilationAsCanceled(Objects);
	for (auto* Object : Objects)
		if (auto* Mesh = Cast<DStaticMesh>(Object))
		{
			AssetPrivate::CancelPhysicsMeshCompilationForOwner(*Mesh);
			if (auto* Body = Mesh->GetBodySetup()) Body->AbortPhysicsMeshAsyncCreation();
		}
	FAssetCompilingManager::Get().FinishCompilationForObjects(Objects);
	for (auto* Object : Objects)
		if (auto* Mesh = Cast<DStaticMesh>(Object))
		{
			AssetPrivate::FinishPhysicsMeshCompilationForOwner(*Mesh);
			if (auto* Body = Mesh->GetBodySetup()) Body->FinishPhysicsMeshes();
		}
	for (FPreparedSceneOutput &Output : Outputs)
	{
		if (Output.Package && IsValid(Output.Package) && Output.Package->IsGraphPrivate())
			MarkObjectHierarchyAsGarbage(Output.Package);
		Output.Candidate = nullptr;
		Output.Package = nullptr;
	}
}

template <typename T>
auto ConstructSceneCandidate(const FTopLevelAssetPath &AssetPath, T *&OutAsset, std::string &OutError) -> bool
{
	OutAsset = nullptr;
	DPackage *Package = NewObject<DPackage>(nullptr, FName(AssetPath.GetPackagePath().GetPackageName()));
	if (!Package || !Package->InitializePreparedAssetPackage(AssetPath.GetPackagePath()))
	{
		OutError = "The scene candidate package could not be created.";
		return false;
	}
	const TStrongObjectPtr<DPackage> CreationOwner(Package);
	FStaticConstructObjectParameters Parameters{T::StaticClass(), Package, FName(AssetPath.GetAssetName()),
	                                            sizeof(T), (Durin::EObjectFlags::Public | Durin::EObjectFlags::Standalone | Durin::EObjectFlags::Transactional)};
	DObject *Object = StaticConstructObject(Parameters);
	DObjectForceRegistration(Object);
	OutAsset = Cast<T>(Object);
	if (!OutAsset || Package->FindTopLevelAsset(OutAsset->GetFName()) != OutAsset)
	{
		MarkObjectHierarchyAsGarbage(Package);
		RequestGarbageCollection();
		OutAsset = nullptr;
		OutError = "The scene candidate could not be registered as a top-level asset.";
		return false;
	}
	Package->MarkDirty();
	Package->MarkAsNewlyCreated();
	return true;
}

// Scene candidates stay private until the complete peer set is
// dependency-bound, validated, and ready for per-package publication.
auto CreateCandidate(FPreparedSceneOutput &Output, std::string &OutError) -> bool
{
	bool bCreated = false;
	FTopLevelAssetPath AssetPath;
	if (!FTopLevelAssetPath::TryCreate(Output.AssetPath, Output.AssetPath.GetPackageName(), AssetPath))
	{
		OutError = "The scene output top-level asset path is invalid.";
		return false;
	}
	const ESceneOutputKind Kind = Output.Descriptor->Kind;
	if (Kind == ESceneOutputKind::StaticMesh)
	{
		DStaticMesh *Value = nullptr;
		bCreated = ConstructSceneCandidate(AssetPath, Value, OutError);
		Output.Candidate = Value;
	}
	else if (Kind == ESceneOutputKind::Material)
	{
		DMaterial *Value = nullptr;
		bCreated = ConstructSceneCandidate(AssetPath, Value, OutError);
		Output.Candidate = Value;
	}
	else if (Kind == ESceneOutputKind::MaterialInstance)
	{
		DMaterialInstance *Value = nullptr;
		bCreated = ConstructSceneCandidate(AssetPath, Value, OutError);
		Output.Candidate = Value;
	}
	else if (Kind == ESceneOutputKind::Texture2D)
	{
		DTexture2D *Value = nullptr;
		bCreated = ConstructSceneCandidate(AssetPath, Value, OutError);
		Output.Candidate = Value;
	}
	if (!bCreated || !Output.Candidate)
	{
		if (OutError.empty())
			OutError = "Scene candidate could not be created.";
		return false;
	}
	Output.CandidatePin = TStrongObjectPtr<DObject>(Output.Candidate);
	Output.Package = Output.Candidate->GetPackage();
	return Output.Package != nullptr;
}

auto ValidateCandidate(const FPreparedSceneOutput &Output, std::string &OutError) -> bool
{
	if (const auto *Mesh = Cast<DStaticMesh>(Output.Candidate))
		return Mesh->GetRenderData() != nullptr;
	if (const auto *Texture = Cast<DTexture2D>(Output.Candidate))
		return Texture->GetPlatformData() != nullptr && Texture->HasPlatformData();
	return Cast<DMaterialInterface>(Output.Candidate) != nullptr;
}
} // namespace

namespace
{
// The game thread is the only publication owner. Admission spans yielded frames.
bool GScenePublicationActive = false;
struct FPublicationAdmission
{
	bool bAcquired = !GScenePublicationActive;
	FPublicationAdmission()
	{
		if (bAcquired)
			GScenePublicationActive = true;
	}
	~FPublicationAdmission()
	{
		if (bAcquired)
			GScenePublicationActive = false;
	}
};
struct FSceneRoutine
{
	struct promise_type
	{
		FAssetImportResult Result;
		auto get_return_object()
		{
			return FSceneRoutine{std::coroutine_handle<promise_type>::from_promise(*this)};
		}
		auto initial_suspend() noexcept
		{
			return std::suspend_always{};
		}
		auto final_suspend() noexcept
		{
			return std::suspend_always{};
		}
		auto return_value(FAssetImportResult Value) -> void
		{
			Result = std::move(Value);
		}
		auto unhandled_exception() -> void
		{
			try
			{
				throw;
			}
			catch (const std::exception &Error)
			{
				Result.Message = Error.what();
			}
			catch (...)
			{
				Result.Message = "Scene import task failed.";
			}
		}
	};
	std::coroutine_handle<promise_type> Handle;
	FSceneRoutine() = default;
	explicit FSceneRoutine(std::coroutine_handle<promise_type> In) : Handle(In)
	{
	}
	FSceneRoutine(FSceneRoutine &&Other) noexcept : Handle(std::exchange(Other.Handle, {}))
	{
	}
	~FSceneRoutine()
	{
		if (Handle)
			Handle.destroy();
	}
};
auto MaterialWorkPending(std::span<DObject *const> Objects) -> bool
{
	return std::ranges::any_of(Objects,
	                           [](DObject *Object)
	                           {
		                           const auto *Material = Cast<DMaterialInterface>(Object);
		                           if (!Material)
			                           return false;
		                           return HasPendingMaterialCompilation(*Material);
	                           });
}
} // namespace

struct FSceneImportState
{
	TWeakObjectPtr<DAssetImportTask> TrackedTask;
	std::vector<TStrongObjectPtr<DObject>> OutputOwners;
	std::vector<DObject*> OutputObjects;
	std::string SourceFile;
	std::string RootFilename;
	FPackagePath DestinationDirectory;
	FStaticMeshImportSettings Settings;
	FSceneMaterialImportOptions MaterialOptions;
	FAssetImportOptions ImportOptions;
	std::optional<FAssetPackageSaveArgs> SaveArgs;
	Testing::FPackageSaveFault SaveFault;
	bool bImportRequested = false;
	bool bTaskAccepted = false;
	bool bReplanRequested = false;
	Tasks::FCancellationToken Cancellation;
	auto CheckCancellation() -> bool { return Cancellation.IsCanceled(); }
	std::function<bool()> IsCancellationRequested;
	FAssetImportProgress Progress;
	FAssetImportResult Result;
	std::optional<FAssetImportResult> Rejection;
	std::shared_ptr<const FSourceSnapshot> Snapshot;
	FAssetImportPlan Data;
	std::vector<FImportOutputSummary> BaseOutputs;
	std::vector<FSceneOutputData> PreviewDescriptors;
	std::vector<FImportOutputSummary> PreviewSummaries;
	FSceneMaterialImportOptions PreviewOptions;
	FPackagePath PreviewDestination;
	bool bPlanResolved = false;
	std::vector<FPreparedSceneOutput> Prepared;
	std::function<bool()> ReadyToResume;
	Tasks::FTask Worker;
	std::optional<FSceneRoutine> Routine;
	bool bCleaning = false;
	bool bWorkerFailed = false;
	std::atomic<size_t> BuiltCount = 0;

	struct FStep
	{
		FSceneImportState &Owner;
		std::function<bool()> Ready;
		auto await_ready() const noexcept -> bool
		{
			return false;
		}
		auto await_suspend(std::coroutine_handle<>) -> void
		{
			Owner.ReadyToResume = std::move(Ready);
		}
		auto await_resume() const -> void
		{
			if (std::exchange(Owner.bWorkerFailed, false))
				throw std::runtime_error("Scene preparation task failed or was canceled.");
		}
	};
	auto Step(std::function<bool()> Ready = {}) -> FStep
	{
		return {*this, std::move(Ready)};
	}
	auto Work(std::function<void()> Body) -> FStep
	{
		Worker = Tasks::LaunchTask("Scene.ImportPreparation", std::move(Body));
		return Step(
		    [this]
		    {
			    if (!Worker.IsCompleted())
				    return false;
			    bWorkerFailed = Worker.GetState() != ETaskState::Succeeded;
			    Worker = {};
			    return true;
		    });
	}
	auto Compilation(const std::vector<DObject *> &Objects) -> FStep
	{
		for (auto *Object : Objects)
			if (auto *Material = Cast<DMaterialInterface>(Object);
			    Material && Material->GetMaterialCompileStatus().HasScheduledEdits())
				RequestMaterialRecompile(*Material);
		return Step(
		    [this, Objects]
		    {
			    Progress.Total = Objects.size();
			    Progress.Completed =
			        std::ranges::count_if(Objects,
			                              [](DObject *Object)
			                              {
				                              const auto *Material = Cast<DMaterialInterface>(Object);
				                              return !Material || !HasPendingMaterialCompilation(*Material);
			                              });
			    return Progress.Completed == Progress.Total;
		    });
	}
	auto Rebase(const FPackagePath &Destination) -> bool
	{
		const auto Prefix = DestinationDirectory.ToString();
		auto Rebased = BaseOutputs;
		for (auto &Output : Rebased)
		{
			FPackagePath NewPath;
			if (!FPackagePath::TryCreate(
			        Destination.ToString() + Output.AssetPath.ToString().substr(Prefix.size()), NewPath))
				return false;
			Output.AssetPath = std::move(NewPath);
		}
		BaseOutputs = std::move(Rebased);
		DestinationDirectory = Destination;
		return true;
	}
	auto ValidateSources(FAssetImportResult& Result) -> bool;
	auto ResolveOutputs(FAssetImportResult& Result, std::vector<FSceneMaterialPreview>& Materials) -> bool;

	auto Tick() -> void;
	auto Run() -> FSceneRoutine;
	auto PrepareSource(FAssetImportResult &Result) -> void;
	auto BuildProducts(FAssetImportResult &Result) -> void;
};

auto FSceneImportState::ResolveOutputs(FAssetImportResult& Result,
	std::vector<FSceneMaterialPreview>& Materials) -> bool
{
	const auto Directory = InspectContentDirectory(DestinationDirectory.ToString());
	if (!Directory) { Result.Message = FormatContentDirectoryValidation(Directory); return false; }
	if (!ConfigureSceneMaterials(Data, Result.Outputs, DestinationDirectory,
		MaterialOptions, Materials, Result.Message))
	{
		for (const auto& Row : Materials)
			if (!Row.bCompatible)
			{
				if (auto Summary = std::ranges::find(Result.Outputs, Row.StableIdentity, &FImportOutputSummary::StableIdentity);
					Summary != Result.Outputs.end()) Summary->Disposition = EImportOutputDisposition::Conflict;
				const bool Occupied = Row.AssetPath.IsValid()
					&& (FindPackageExact(Row.AssetPath) || FindResidentPackage(Row.AssetPath));
				Result.Diagnostics.push_back({.Category = Occupied ? EImportDiagnosticCategory::Collision : EImportDiagnosticCategory::InvalidPlan,
					.Phase = "asset-preflight", .OutputIdentity = Row.StableIdentity, .Message = Row.Message});
			}
		return false;
	}
	if (!ImportOptions.bIncludeUnusedResources)
	{
		std::unordered_set<std::string> RequiredTextures;
		for (const auto& Output : Data.Outputs)
			if (IsSceneMaterial(Output.Kind))
				for (const auto& Identity : Output.Dependencies) RequiredTextures.insert(Identity);
		std::unordered_set<std::string> Omitted;
		std::erase_if(Data.Outputs, [&](const auto& Output) {
			const bool bOmit = Output.Kind == ESceneOutputKind::Texture2D && !RequiredTextures.contains(Output.StableIdentity);
			if (bOmit) Omitted.insert(Output.StableIdentity);
			return bOmit;
		});
		std::erase_if(Result.Outputs, [&](const auto& Output) { return Omitted.contains(Output.StableIdentity); });
	}
	if (Result.Outputs.empty()) { Result.Message = "No asset outputs are selected."; return false; }
	bool bValid = true;
	std::unordered_set<std::string> Destinations;
	for (auto& Summary : Result.Outputs)
	{
		auto Descriptor = std::ranges::find(Data.Outputs, Summary.StableIdentity, &FSceneOutputData::StableIdentity);
		std::string Error;
		if (Descriptor->bExistingMaterialMapping)
		{
			auto* Material = Descriptor->PreservedMaterial.Get();
			Summary.Disposition = EImportOutputDisposition::Preserve;
			Descriptor->PreviousRevision = Material->GetMaterialContentRevision();
			continue;
		}
		if (FindPackageExact(Summary.AssetPath) || FindResidentPackage(Summary.AssetPath))
			Error = "Output destination is occupied. One-time model imports require a new destination.";
		if (Error.empty())
		{
			const auto Destination = InspectAssetDestination(Summary.AssetPath.ToString());
			if (!Destination) Error = FormatAssetDestinationValidation(Destination);
		}
		if (!Destinations.insert(Summary.AssetPath.ToString()).second) Error = "Two outputs resolve to the same destination.";
		if (!Error.empty())
		{
			bValid = false;
			Summary.Disposition = EImportOutputDisposition::Conflict;
			Result.Diagnostics.push_back({.Category = EImportDiagnosticCategory::Collision, .Phase = "asset-preflight",
				.SourceIdentity = "root", .OutputIdentity = Summary.StableIdentity, .Message = Error});
			if (Result.Message.empty()) Result.Message = Error;
		}
	}
	return bValid;
}

auto FSceneImportState::PrepareSource(FAssetImportResult &Result) -> void
{
	Private::FScopedSceneImportCancellation CancellationScope(IsCancellationRequested);
	RootFilename = std::filesystem::absolute(SourceFile).lexically_normal().generic_string();
	FSourceSnapshotBuilder SnapshotBuilder(IsCancellationRequested);
	if (!SnapshotBuilder.CaptureRootFilename(RootFilename, Result.Diagnostics) ||
	    !SnapshotBuilder.DiscoverSourceDependencies(
	        [](std::span<const FSourceSnapshotEntry> Sources, FDependencyRequestSink &Sink,
	           std::vector<FImportDiagnostic> &Diagnostics)
	        { return DiscoverSceneImportDependencies(Sources, Sink, Diagnostics); },
	        Result.Diagnostics))
	{
		Result = AddError(Result, EImportDiagnosticCategory::InvalidSource, "scene-capture",
		                  "Scene source closure could not be captured.");
		return;
	}
	Snapshot = SnapshotBuilder.Freeze(Result.Diagnostics);
	if (!Snapshot)
	{
		Result = AddError(Result, EImportDiagnosticCategory::InvalidSource, "scene-capture",
		                  "Scene source closure could not be finalized.");
		return;
	}
	if (IsCanceled(IsCancellationRequested))
	{
		Result = AddError(Result, EImportDiagnosticCategory::Canceled, "scene-translation",
		                  "Scene import was canceled before translation.");
		return;
	}

	if (!BuildScenePlan(*Snapshot, DestinationDirectory, Settings, Data, Result.Outputs, Result.Diagnostics,
	                    Result.Message, ImportOptions))
	{
		if (Result.Message.empty() && !Result.Diagnostics.empty() && !Result.Diagnostics.back().Message.empty())
			Result.Message = Result.Diagnostics.back().Message;
		return;
	}
}

auto FSceneImportState::ValidateSources(FAssetImportResult& Result) -> bool
{
	// Validate the captured closure without parsing the source a second time.
	for (const auto &Source : Snapshot->GetSources())
		{
			if (IsCanceled(IsCancellationRequested))
			{
				Result = AddError(Result, EImportDiagnosticCategory::Canceled, "source-validation", "Asset import canceled.");
				return false;
			}
			const auto Hash = FFileHelper::HashFileXx128(Source.Filename);
			if (!Hash || *Hash != Source.ContentHash)
			{
				Result = AddError(Result, EImportDiagnosticCategory::InvalidSource, "source-validation",
					"Source changed after preparation; refresh the plan.");
				return false;
			}
		}
	return true;
}

auto FSceneImportState::BuildProducts(FAssetImportResult &Result) -> void
{
	Private::FScopedSceneImportCancellation CancellationScope(IsCancellationRequested);
	if (!ValidateSources(Result)) return;
	std::vector<size_t> OutputOrder;
	if (!MakeStableOutputOrder(Data, OutputOrder, Result.Message))
	{
		Result = AddError(Result, EImportDiagnosticCategory::DependencyCycle, "scene-order", Result.Message);
		return;
	}
	Prepared.reserve(Data.Outputs.size());
	const FSourceSnapshotEntry *Root = Snapshot->FindSource("root");
	if (!Root)
	{
		Result = AddError(Result, EImportDiagnosticCategory::InvalidSource, "scene-build",
		                  "Scene root source is unavailable.");
		return;
	}
	for (const size_t Index : OutputOrder)
	{
		if (IsCanceled(IsCancellationRequested))
		{
			Result = AddError(Result, EImportDiagnosticCategory::Canceled, "scene-build",
			                  "Scene import was canceled during product construction.");
			return;
		}
		const FSceneOutputData &Descriptor = Data.Outputs[Index];
		const auto Summary = std::ranges::find(Result.Outputs, Descriptor.StableIdentity,
		                                       &FImportOutputSummary::StableIdentity);
		if (Summary == Result.Outputs.end())
		{
			Result = AddError(Result, EImportDiagnosticCategory::InvalidPlan, "scene-build",
			                  "Scene output mapping is incomplete.", Descriptor.StableIdentity);
			return;
		}
		FPreparedSceneOutput &Output = Prepared.emplace_back();
		Output.Descriptor = &Descriptor;
		Output.AssetPath = Summary->AssetPath;
		std::string Error;
		if (Descriptor.Kind == ESceneOutputKind::Texture2D)
		{
			if (!BuildSceneImportTextureProduct(*Snapshot, Data, Descriptor, IsCancellationRequested,
			                                    Output.Texture, Error))
			{
				Result = AddError(Result, EImportDiagnosticCategory::CandidateFailure, "scene-build",
				                  std::move(Error), Descriptor.StableIdentity);
				return;
			}
		}
		else if (Descriptor.Kind == ESceneOutputKind::StaticMesh)
		{
			FImportedSceneData Selected;
			if (!Descriptor.bCombinedMesh && !SelectImportedMeshResource(Data.Document, Descriptor.SourceIndex, Selected, Error))
			{
				Result = AddError(Result, EImportDiagnosticCategory::InvalidPlan, "asset-build", Error, Descriptor.StableIdentity);
				return;
			}
			auto Geometry = MakeMeshDescription(Descriptor.bCombinedMesh ? Data.Scene : Selected);
			Output.StaticMeshMaterialSlots = ReconcileStaticMeshMaterialSlots({}, Geometry.PolygonGroups);
			if (const auto Initialized =
			        Output.StaticMeshSource.Initialize(std::move(Geometry));
			    !Initialized)
			{
				Result = AddError(Result, EImportDiagnosticCategory::CandidateFailure, "scene-build",
				                  FormatStaticMeshSourceError(Initialized.error()), Descriptor.StableIdentity);
				return;
			}
			FStaticMeshBuildRequest Request{.Settings = MakeStaticMeshBuildSettings(Output.StaticMeshMaterialSlots),
				.Source = Output.StaticMeshSource};
			if (CheckCancellation())
			{
				Result = AddError(Result, EImportDiagnosticCategory::Canceled, "scene-build",
				    "Scene mesh build was cancelled.", Descriptor.StableIdentity);
				return;
			}
			auto Outcome = BuildStaticMeshRenderData(std::move(Request));
			if (CheckCancellation() || !Outcome)
			{
				Result = AddError(Result,
				                  CheckCancellation()
				                      ? EImportDiagnosticCategory::Canceled
				                      : EImportDiagnosticCategory::CandidateFailure,
				                  "scene-build", CheckCancellation() ? "Scene mesh build was cancelled." : Outcome.error(),
				                  Descriptor.StableIdentity);
				return;
			}
			Output.StaticMesh = std::move(*Outcome);
		}
		++BuiltCount;
	}
}

auto FSceneImportState::Run() -> FSceneRoutine
{
	FAssetImportResult Result;
	try
	{
		if (IsCanceled(IsCancellationRequested))
			co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-request",
			                   "Scene import canceled.");
		if (SourceFile.empty() || !DestinationDirectory.IsValid() || !Settings.Validate())
			co_return AddError(Result, EImportDiagnosticCategory::InvalidRequest, "scene-request",
			                   "Scene import request is invalid.");
		Progress = {EAssetImportPhase::Reading, "Reading scene and dependencies"};
		co_await Work([&] { PrepareSource(Result); });
		if (IsCanceled(IsCancellationRequested))
			co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-prepare",
			                   "Scene import canceled.");
		if (!Result.Message.empty() || !Snapshot)
			co_return std::move(Result);
		BaseOutputs = Result.Outputs;
		while (!bImportRequested)
		{
			Progress = {EAssetImportPhase::Ready, "Configure asset outputs"};
			co_await Step([this] { return bImportRequested || bReplanRequested || CheckCancellation(); });
			if (CheckCancellation())
				co_return AddError(Result, EImportDiagnosticCategory::Canceled, "asset-preview", "Asset import canceled.");
			if (std::exchange(bReplanRequested, false))
			{
				Progress = {EAssetImportPhase::Reading, "Resolving selected resources"};
				co_await Work([&] {
					auto Document = std::move(Data.Document);
					Result.Diagnostics.clear();
					Result.Message.clear();
					if (!BuildScenePlan(*Snapshot, DestinationDirectory, Settings, Data, Result.Outputs,
						Result.Diagnostics, Result.Message, ImportOptions, &Document)) return;
				});
				if (!Result.Message.empty()) co_return std::move(Result);
				BaseOutputs = Result.Outputs;
			}
		}
		Result.Outputs = BaseOutputs;
		std::vector<FSceneMaterialPreview> MaterialPreview;
		if (!bPlanResolved && !ResolveOutputs(Result, MaterialPreview))
			co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure, "scene-material-policy",
			                   Result.Message);		Progress = {EAssetImportPhase::Building, "Building textures and mesh", 0, Data.Outputs.size()};
		co_await Work([&] { BuildProducts(Result); });
		if (IsCanceled(IsCancellationRequested))
			co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-build",
			                   "Scene import canceled.");
		if (!Result.Message.empty())
			co_return std::move(Result);

		for (auto &Output : Prepared)
			Output.AssetPath = std::ranges::find(Result.Outputs, Output.Descriptor->StableIdentity,
			                                     &FImportOutputSummary::StableIdentity)
			                       ->AssetPath;
		if (IsCanceled(IsCancellationRequested))
			co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-publication",
			                   "Scene import was canceled before publication.");
		FPublicationAdmission PublicationLock;
		if (!PublicationLock.bAcquired)
			co_return AddError(Result, EImportDiagnosticCategory::InvalidRequest, "scene-publication",
			                   "Another scene import is publishing assets.");
		Progress = {EAssetImportPhase::Preparing, "Preparing assets", 0, Prepared.size()};
		for (FPreparedSceneOutput &Output : Prepared)
		{
			Progress.Activity = Output.AssetPath.ToString();
			Progress.Completed = static_cast<size_t>(&Output - Prepared.data());
			co_await Step();
			if (IsCanceled(IsCancellationRequested))
				co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-prepare",
				                   "Scene import canceled.");
			if (Output.Descriptor->bExistingMaterialMapping)
			{
				auto* Mapped = Output.Descriptor->PreservedMaterial.Get();
				if (!Mapped || Mapped->GetMaterialContentRevision() != Output.Descriptor->PreviousRevision)
					co_return AddError(Result, EImportDiagnosticCategory::Collision, "asset-mapping", "Mapped material changed after preflight.", Output.Descriptor->StableIdentity);
				continue;
			}
			if (FindPackageExact(Output.AssetPath) || FindResidentPackage(Output.AssetPath))
			{
				co_return AddError(Result, EImportDiagnosticCategory::Collision, "scene-publication",
					"Output destination changed after preflight.", Output.Descriptor->StableIdentity);
			}
			std::ranges::find(Result.Outputs, Output.Descriptor->StableIdentity,
			                  &FImportOutputSummary::StableIdentity)
			    ->AssetPath = Output.AssetPath;
		}

		for (FPreparedSceneOutput &Output : Prepared)
		{
			Progress.Activity = Output.AssetPath.ToString();
			Progress.Completed = static_cast<size_t>(&Output - Prepared.data());
			co_await Step();
			if (IsCanceled(IsCancellationRequested))
				co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-prepare",
				                   "Scene import canceled.");
			std::string Error;
			if (Output.Descriptor->PreservedMaterial.Get())
			{
				Output.Candidate = Output.Descriptor->PreservedMaterial.Get();
				Output.Package = Output.Candidate->GetPackage();
				std::ranges::find(Result.Outputs, Output.Descriptor->StableIdentity,
					&FImportOutputSummary::StableIdentity)->State = EImportOutputState::Preserved;
				continue;
			}
			if (!CreateCandidate(Output, Error))
			{
				co_return AddError(Result, EImportDiagnosticCategory::CandidateFailure,
				                   "scene-materialization", std::move(Error),
				                   Output.Descriptor->StableIdentity);
			}
			const FSceneOutputData &Descriptor = *Output.Descriptor;
			if (Descriptor.Kind == ESceneOutputKind::Texture2D)
			{
				const FXxHash128 SourceHash = Output.Texture.EncodedSourceHash;
				const std::string SourcePhysicalPath = Output.Texture.SourceFilename;
				const auto PackageResolution = FMountPaths::ResolveAssetPath(
				    Output.AssetPath.GetView(), EMountPathExistence::AllowMissing);
				if (!PackageResolution)
				{
					co_return AddError(
					    Result, EImportDiagnosticCategory::CandidateFailure, "scene-materialization",
					    (PackageResolution ? std::string{} : ToString(PackageResolution.error())),
					    Descriptor.StableIdentity);
				}
				std::filesystem::path PackagePath = PackageResolution->PhysicalPath;
				PackagePath += ".dasset";
				std::string SourceHint;
				ESourceHintBase HintBase;
				if (auto Hint = MakeSourceHint(SourcePhysicalPath, PackagePath.generic_string()); !Hint)
				{
					co_return AddError(Result, EImportDiagnosticCategory::CandidateFailure,
					                   "scene-materialization", FormatSourceHintError(Hint.error()),
					                   Descriptor.StableIdentity);
				}
				else
				{
					HintBase = Hint->Base;
					SourceHint = std::move(Hint->Hint);
				}
				auto *Texture = Cast<DTexture2D>(Output.Candidate);
				FTexturePlatformData &Product = Output.Texture.PlatformData;
				const FTexture2DBuildSettings &Settings = Output.Texture.Settings;
				auto PlatformData = std::make_unique<FTexturePlatformData>(std::move(Product));
				if (!PlatformData->IsValid())
				{
					co_return AddError(Result, EImportDiagnosticCategory::CandidateFailure,
					                   "scene-materialization", "Texture platform data is invalid.",
					                   Descriptor.StableIdentity);
				}
				if (const auto Validation = ValidateTexture2DBuildSettings(Settings); !Validation)
				{
					co_return AddError(Result, EImportDiagnosticCategory::CandidateFailure,
					                   "scene-materialization", FormatTexture2DInputError(Validation.error()),
					                   Descriptor.StableIdentity);
				}
				Texture->SetSource(Output.Texture.SourceData);
				Texture->SetBuildSettings(Settings.Usage, ResolveTexture2DSRGB(Settings),
				                          Settings.MaxResolution, Settings.CompressionQuality,
				                          Settings.AlphaMipMode, Settings.AlphaCoverageThreshold);
				Texture->SetPlatformData(std::move(PlatformData));
				Texture->UpdateResource();
				Texture->MarkPackageDirty();
				FAssetImportDataState ImportState;
				ImportState.SourceData.Sources.push_back(
				    {.Role = "source",
				     .DisplayLabel = std::filesystem::path(SourcePhysicalPath).filename().generic_string(),
				     .Hint = SourceHint,
				     .HintBase = HintBase,
				     .ContentHashLow = SourceHash.HashLow,
				     .ContentHashHigh = SourceHash.HashHigh,
				     .ByteCount = Output.Texture.SourceFileSize});
				auto *ImportData = NewObject<DSceneImportData>(Output.Candidate, "AssetImportData");
				ImportState.SourceData.Normalize();
				const auto Validation = ImportState.Validate();
				if (!ImportData || !Validation)
				{
					co_return AddError(Result, EImportDiagnosticCategory::CandidateFailure,
					                   "scene-materialization",
					                   !ImportData ? "Scene texture import data could not be published."
					                               : FormatAssetImportDataError(Validation.error()),
					                   Descriptor.StableIdentity);
				}
				ImportData->SourceIdentity = RootFilename;
				ImportData->OutputIdentity = Descriptor.StableIdentity;
				ImportData->SetState(std::move(ImportState));
				Texture->SetAssetImportData(*ImportData);
				Output.Candidate->MarkPackageDirty();
			}
			else if (Descriptor.Kind == ESceneOutputKind::StaticMesh)
			{
				auto *Mesh = Cast<DStaticMesh>(Output.Candidate);
				auto *ImportData = NewObject<DSceneImportData>(Mesh, "AssetImportData");
				if (!ImportData)
				{
					co_return AddError(
					    Result, EImportDiagnosticCategory::CandidateFailure, "scene-materialization",
					    "Scene mesh import data could not be created.", Descriptor.StableIdentity);
				}
				ImportData->SourceIdentity = RootFilename;
				ImportData->OutputIdentity = Descriptor.StableIdentity;
				if (const auto Applied = CommitStaticMeshBuild(
				        *Mesh, std::move(Output.StaticMesh), Output.StaticMeshSource, CaptureStaticMeshReconciliation(*Mesh), true,
				        ImportData, &Output.StaticMeshMaterialSlots);
				    !Applied)
				{
					co_return AddError(
					    Result, EImportDiagnosticCategory::CandidateFailure, "scene-materialization",
					    Applied.error(), Descriptor.StableIdentity);
				}
			}
		}

		auto FindOutput = [&](std::string_view Identity) -> FPreparedSceneOutput *
		{
			const auto It = std::ranges::find_if(Prepared, [&](const FPreparedSceneOutput &Value)
			                                     { return Value.Descriptor->StableIdentity == Identity; });
			return It == Prepared.end() ? nullptr : &*It;
		};

		for (FPreparedSceneOutput &Output : Prepared)
		{
			Progress.Activity = Output.AssetPath.ToString();
			Progress.Completed = static_cast<size_t>(&Output - Prepared.data());
			co_await Step();
			if (IsCanceled(IsCancellationRequested))
				co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-prepare",
				                   "Scene import canceled.");
			const FSceneOutputData &Descriptor = *Output.Descriptor;
			std::string Error;
			if (Descriptor.PreservedMaterial.Get())
				continue;
			if (IsSceneMaterial(Descriptor.Kind))
			{
				const auto Imported = std::ranges::find(Data.Scene.Materials, Descriptor.SourceIndex,
				                                        &FImportedMaterial::SourceMaterialIndex);
				const auto Roles = MakeSceneSurfaceRoles(Data, Descriptor);
				auto Recipe = MakeImportedSurfaceRecipe(Roles);
				const auto StandardDefinitions = GetPBRMaterialParameterDefinitions();
				if (Descriptor.bStandardPBRParent)
				{
					Recipe.Owners.clear();
					for (uint32 Role = 0; Role < Roles.size(); ++Role)
						for (uint32 Kind = 0; Kind < MaterialParameters::BuiltinParameterKindCount; ++Kind)
						{
							const auto Surface = MaterialParameters::PBRMaterialProperties.at(Role);
							const auto ParameterKind =
							    static_cast<MaterialParameters::EMaterialBuiltinParameterKind>(Kind);
							const auto Id = GetMaterialSurfaceParameterId(Surface, ParameterKind);
							const auto It = std::ranges::find(StandardDefinitions, Id, &FMaterialParameterDefinition::Id);
							require(It != StandardDefinitions.end());
							Recipe.Owners.push_back({Surface, ParameterKind, It->GetParameterInfo()});
						}
				}
				auto *Standard = Descriptor.Kind == ESceneOutputKind::Material
				                     ? Cast<DMaterial>(Output.Candidate)
				                     : Descriptor.Parent.Get();
				if (Descriptor.Kind == ESceneOutputKind::Material && Standard)
				{
					std::unordered_map<FGuid, FGuid> ParameterIds;
					for (const auto& Expression : Recipe.Graph.Expressions)
						if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
						{
							auto [Binding, Inserted] = ParameterIds.try_emplace(Parameter->Metadata.Id);
							if (Inserted) Binding->second = FGuid::NewGuid();
							Parameter->Metadata.Id = Binding->second;
						}
					Standard->SetEditCompileMode(EMaterialEditCompileMode::Manual);
					if (!Recipe.Graph.Apply(*Standard))
					{
						co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure,
						                   "scene-material-graph",
						                   "Could not create the imported material graph.");
					}
				}
				if (Imported == Data.Scene.Materials.end() || !Standard)
				{
					co_return AddError(
					    Result, EImportDiagnosticCategory::MissingDependency, "scene-dependency-binding",
					    Error.empty() ? "Scene material dependency is unavailable." : std::move(Error),
					    Descriptor.StableIdentity);
				}
				auto *Material = Cast<DMaterialInterface>(Output.Candidate);
				auto *Instance = Cast<DMaterialInstance>(Material);
				FMaterialPropertyOverrides Overrides;
				Overrides.bOverrideBlendMode = true;
				Overrides.bOverrideOpacityMaskThreshold = true;
				Overrides.bOverrideTwoSided = true;
				auto &Properties = Overrides.Values;
				Properties.BlendMode =
				    Imported->AlphaMode == EImportedAlphaMode::Mask    ? EMaterialBlendMode::Masked
				    : Imported->AlphaMode == EImportedAlphaMode::Blend ? EMaterialBlendMode::Translucent
				                                                       : EMaterialBlendMode::Opaque;
				Properties.bTwoSided = Imported->bDoubleSided;
				Properties.OpacityMaskThreshold = Imported->AlphaCutoff;
				if (!Material || !(Instance ? Instance->SetParentAndPropertyOverrides(Standard, Overrides)
				                            : static_cast<bool>(Standard->SetStaticProperties(Properties))))
				{
					co_return AddError(
					    Result, EImportDiagnosticCategory::MissingDependency, "scene-dependency-binding",
					    "Scene material parent could not be applied.", Descriptor.StableIdentity);
				}
				FMaterialImportProvenance Receipt{.RecipeId = "Durin.ImportedSurface",
				                                  .RecipeVersion = 1,
				                                  .StructuralKey = Recipe.CanonicalKey,
				                                  .SourceIdentity = RootFilename,
				                                  .OutputIdentity = Descriptor.StableIdentity};
				for (const auto &Owner : Recipe.Owners)
				{
					using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
					const auto StandardDefinition = std::ranges::find(StandardDefinitions, Owner.ParameterInfo.Name,
					                                                  &FMaterialParameterDefinition::Name);
					require(StandardDefinition != StandardDefinitions.end());
					const auto* Definition = Standard->FindParameterDefinitionByInfo(Owner.ParameterInfo);
					if (!Definition || Definition->Type != StandardDefinition->Type)
						co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure, "scene-material-parameters",
						                   "The material no longer exposes the expected PBR parameter.", Descriptor.StableIdentity);
					const auto &Role = Roles[static_cast<uint32>(Owner.Role)];
					const auto *Sample = Role.Sample ? &*Role.Sample : nullptr;
					// The full template reads opacity from alpha, while compact imports use
					// the derived red-channel mask. Alpha is unaffected by the source's sRGB view.
					if (Descriptor.bStandardPBRParent && Owner.Role == EMaterialProperty::MP_Opacity &&
					    Sample)
						Sample = &*Roles[0].Sample;
					FMaterialParameterValue Value;
					if (Descriptor.bStandardPBRParent && Owner.Kind != Kind::Value && !Sample)
						Value = StandardDefinition->Value;
					else if (Owner.Kind == Kind::Texture)
					{
						auto *Texture = FindOutput(Sample->ResourceIdentity);
						require(Texture && Cast<DTexture2D>(Texture->Candidate));
						Value = FMaterialParameterValue::MakeTexture(
						    Cast<DTexture2D>(Texture->Candidate), Sample->Sampler,
						    Definition->Value.GetTexture().TextureFallback);
					}
					else
					{
						const auto Literal =
						    Owner.Kind == Kind::Value
						        ? (Descriptor.bStandardPBRParent &&
						                   Owner.Role == EMaterialProperty::MP_EmissiveColor && Sample
						               ? FMaterialProgramLiteral{}
						               : Role.Value)
						    : Owner.Kind == Kind::UVChannel ? Sample->UVChannel
						    : Owner.Kind == Kind::UVScale   ? Sample->UVScale
						    : Owner.Kind == Kind::UVOffset  ? Sample->UVOffset
						                                    : Sample->UVRotation;
						Value = Definition->Type == EMaterialParameterType::Vector4
						            ? FMaterialParameterValue::MakeVector4(
						                  {Literal.X, Literal.Y, Literal.Z, Literal.W})
						            : FMaterialParameterValue::MakeScalar(Literal.X);
					}
					if (const auto Applied = Instance ? Instance->SetParameterValueByInfo(Definition->GetParameterInfo(), Value)
					                                  : Standard->SetParameterValueByInfo(Definition->GetParameterInfo(), Value);
					    !Applied)
					{
						co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure,
						                   "scene-material-parameters", Applied.error(),
						                   Descriptor.StableIdentity);
					}
				}
				if (!Material->SetImportProvenance(std::move(Receipt)))
				{
					co_return AddError(
					    Result, EImportDiagnosticCategory::ValidationFailure, "scene-material-parameters",
					    "Scene material import receipt is invalid.", Descriptor.StableIdentity);
				}
			}
			else if (Descriptor.Kind == ESceneOutputKind::StaticMesh)
			{
				auto *Mesh = Cast<DStaticMesh>(Output.Candidate);
				for (const FSceneOutputData &Candidate : Data.Outputs)
					if (IsSceneMaterial(Candidate.Kind) && std::ranges::contains(Descriptor.Dependencies, Candidate.StableIdentity))
					{
						FPreparedSceneOutput *Material = FindOutput(Candidate.StableIdentity);
						uint32 SlotIndex = 0;
						if (!Material ||
						    !ResolveImportedMaterialSlot(*Mesh, Candidate.SourceIndex, SlotIndex, Error))
						{
							co_return AddError(Result, EImportDiagnosticCategory::MissingDependency,
							                   "scene-dependency-binding",
							                   Error.empty() ? "Scene material dependency is unavailable."
							                                 : std::move(Error),
							                   Descriptor.StableIdentity);
						}
						Mesh->SetMaterialSlotDefaultMaterial(SlotIndex,
						                                     Cast<DMaterialInterface>(Material->Candidate));

					}
			}
			Output.Candidate->MarkPackageDirty();
			if (!ValidateCandidate(Output, Error))
			{
				co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure, "scene-validation",
				                   Error.empty() ? "Scene candidate has no validated runtime data."
				                                 : std::move(Error),
				                   Descriptor.StableIdentity);
			}
		}

		Progress = {EAssetImportPhase::Compiling, "Compiling materials"};
		std::vector<DObject *> Materials;
		for (const auto &Output : Prepared)
		{
			if (Output.Descriptor->PreservedMaterial.Get())
				continue;
			auto *Material = Cast<DMaterialInterface>(Output.Candidate);
			if (!Material)
				continue;
			const bool bCompiled = Cast<DMaterial>(Material) ? Cast<DMaterial>(Material)->CompileEdits()
			                                                 : RequestMaterialRecompile(*Material);
			if (!bCompiled)
			{
				co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure,
				                   "scene-material-compile",
				                   "Private material candidate could not be compiled.");
			}
			Materials.push_back(Material);
		}
		co_await Compilation(Materials);
		for (auto *Object : Materials)
			if (auto *Material = Cast<DMaterial>(Object))
				Material->SetEditCompileMode(EMaterialEditCompileMode::Immediate);
		for (auto *Object : Materials)
		{
			auto *Material = Cast<DMaterialInterface>(Object);
			if (Material->GetMaterialCompileStatus().IsCurrent() && Material->GetAcceptedCompiledProgram())
				continue;
			const auto Diagnostics = Material->GetMaterialCompileDiagnostics();
			const std::string Message =
			    Diagnostics.empty()
			        ? std::format("Scene material variant is not ready: {} (state {}).",
			                      Material->GetObjectPath(),
			                      static_cast<uint32>(Material->GetMaterialCompileStatus().State))
			        : Diagnostics.front().Message;
			co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure, "scene-material-compile",
			                   Message);
		}

		co_await Work([&] { (void)ValidateSources(Result); });
		if (!Result.Message.empty()) co_return std::move(Result);

		if (IsCanceled(IsCancellationRequested))
		{
			co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-publication",
			                   "Scene import was canceled before applying assets.");
		}

		std::vector<FObjectReplacementPackagePair> Pairs;
		for (const auto &Output : Prepared)
			if (!Output.Descriptor->PreservedMaterial.Get())
				Pairs.push_back({nullptr, Output.Package});
		if (!Pairs.empty())
		{
			Progress = {EAssetImportPhase::Applying, "Applying imported assets"};
			FObjectGraphReplacement Publication;
			const std::array Participants{MakeMaterialReferenceReplacementParticipant()};
			auto Applied = Publication.Prepare(Pairs, Participants);
			if (Applied) Applied = Publication.TryCommit();
			if (!Applied)
			{
				Publication.Abort();
				co_return AddError(Result, EImportDiagnosticCategory::CandidateFailure, "scene-apply",
				                   ToString(Applied.error()));
			}
			for (const auto& Pair : Pairs)
			{
				Pair.Prepared->MarkDirty();
				Result.ImportedPackages.push_back(Pair.Prepared->GetPackagePathIdentity());
				std::ranges::find(Result.Outputs, Pair.Prepared->GetPackagePathIdentity(),
					&FImportOutputSummary::AssetPath)->State = EImportOutputState::Imported;
			}
			co_await Step([&] { return Publication.Retire(); });
		}
		Result.bSucceeded = true;

		if (SaveArgs)
		{
			auto& Batch = Result.SaveResult.emplace();
			for (size_t Index = 0; Index < Pairs.size(); ++Index)
			{
				auto* Package = Pairs[Index].Prepared;
				Progress = {EAssetImportPhase::Saving, Package->GetPackagePath(), Index, Pairs.size()};
				if (IsCanceled(IsCancellationRequested))
					Batch.Result = {.Error = UnifiedError::FError{AssetWriteErrors::Cancelled, "Save was cancelled after importing assets."}};
				else
				{
					const auto Fault = [Captured = SaveFault, Index](Testing::EPackageSaveFaultPoint Phase, size_t) {
						return Captured && Captured(Phase, Index);
					};
					Tasks::TTask<FAssetWriteResult> Task;
					{
						Testing::FScopedPackageSaveFault Scope(Fault);
						Task = SavePackageAsync(Package, *SaveArgs, IsCancellationRequested);
					}
					co_await Step([&] { return Task.IsCompleted(); });
					const auto State = Task.GetState();
					if (State == ETaskState::Succeeded)
						Batch.Result = Task.GetResult();
					else if (State == ETaskState::Canceled)
						Batch.Result = {.Error = UnifiedError::FError{AssetWriteErrors::Cancelled, "Save task was cancelled."}};
					else
						Batch.Result = {.Error = UnifiedError::FError{AssetWriteErrors::IoError,
							std::format("Save task failed: {}", Task.GetDiagnostic())}, .Effect = EAssetWriteEffect::ContentUncertain};
				}
				if (!Batch.Result)
				{
					Batch.FailedPackage = Package->GetPackagePathIdentity();
					const std::string_view Disposition = Batch.Result.Effect == EAssetWriteEffect::ContentCommittedProjectionPending
						? "Current package content was saved; the asset catalog requires synchronization. "
						: Batch.Result.Effect == EAssetWriteEffect::None ? ""
						: "Current package files may have changed; inspect recovery details. ";
					Result.Message = std::format("Import complete; saved {} of {} packages; save failed for {}: {}{}",
						Batch.SavedPackages.size(), Pairs.size(), Batch.FailedPackage.ToString(), Disposition,
						Batch.Result.Error ? Batch.Result.Error->ToString() : std::string{});
					Result.Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::PersistenceFailure, .Phase = "scene-save",
						.Message = Result.Message});
					break;
				}
				Batch.SavedPackages.push_back(Package->GetPackagePathIdentity());
			}
		}
		co_return Result;
	}
	catch (const std::exception &Error)
	{
		co_return AddError(Result, EImportDiagnosticCategory::CandidateFailure, "scene-task", Error.what());
	}
	catch (...)
	{
		co_return AddError(Result, EImportDiagnosticCategory::CandidateFailure, "scene-task",
		                   "Scene task failed.");
	}
}

auto FSceneImportState::Tick() -> void
{
	if (Progress.Phase == EAssetImportPhase::Building)
		Progress.Completed = BuiltCount.load();
	if (Progress.Phase == EAssetImportPhase::Completed)
		return;
	if (!Routine)
		Routine.emplace(Run());
	if (!Routine->Handle.done())
	{
		if (ReadyToResume && !ReadyToResume())
			return;
		ReadyToResume = {};
		Routine->Handle.resume();
	}
	if (!Routine->Handle.done())
		return;
	if (!bCleaning)
	{
		Result = std::move(Routine->Handle.promise().Result);
		if (Rejection) { Result = std::move(*Rejection); Rejection.reset(); }
		bCleaning = true;
		Progress = {EAssetImportPhase::Finishing, "Releasing import resources"};
		std::vector<DObject *> Objects;
		for (const auto &Output : Prepared)
			if (Output.Package && IsValid(Output.Package) && Output.Package->IsGraphPrivate())
				Objects.push_back(Output.Candidate);
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(Objects);
	}
	std::vector<DObject *> Objects;
	for (const auto &Output : Prepared)
		if (Output.Package && IsValid(Output.Package) && Output.Package->IsGraphPrivate())
			Objects.push_back(Output.Candidate);
	if (MaterialWorkPending(Objects))
		return;
	for (const auto& Output : Prepared)
		if (Output.Package && IsValid(Output.Candidate) && !Output.Package->IsGraphPrivate()
			&& std::ranges::contains(Result.ImportedPackages, Output.AssetPath))
		{ OutputOwners.emplace_back(Output.Candidate); OutputObjects.push_back(Output.Candidate); }
	Abandon(Prepared);
	Prepared.clear();
	Data = {};
	Snapshot.reset();
	Routine.reset();
	Progress = {EAssetImportPhase::Completed, Result.Message.empty() ? "Import complete" : Result.Message};
}

FSceneImportPreview::FSceneImportPreview(std::string SourceFile, FPackagePath Destination,
                                         FStaticMeshImportSettings Settings, FAssetImportOptions Options)
    : Impl(std::make_shared<FSceneImportState>())
{
	Impl->SourceFile = std::move(SourceFile);
	Impl->DestinationDirectory = std::move(Destination);
	Impl->Settings = std::move(Settings);
	Impl->ImportOptions = std::move(Options);
	Impl->IsCancellationRequested = [State = Impl.get()] { return State->CheckCancellation(); };
}
FSceneImportPreview::~FSceneImportPreview()
{
	Cancel();
	if (auto* Task = Impl->TrackedTask.Get(); Task && !Task->IsAsyncImportComplete())
	{ (void)Task->GetObjects(); return; }
	// Only host teardown drains synchronously; normal UI cancellation keeps ticking.
	while (Impl->Progress.Phase != EAssetImportPhase::Completed)
	{
		PumpGameThreadDeferredWork();
		FAssetCompilingManager::Get().ProcessAsyncTasks(false);
		Impl->Tick();
		if (Impl->Progress.Phase != EAssetImportPhase::Completed)
			std::this_thread::yield();
	}
}
auto FSceneImportPreview::Tick() -> void
{
	if (!Impl->bTaskAccepted && !Impl->bImportRequested) Impl->Tick();
	// Phase transitions can replace Progress; publish the cancellation fact afterwards.
	Impl->Progress.bCancellationRequested = Impl->CheckCancellation();
}
auto FSceneImportPreview::Cancel() -> void
{
	Impl->Cancellation.Cancel();
	Impl->Progress.bCancellationRequested = true;
}
auto FSceneImportPreview::GetProgress() const -> const FAssetImportProgress &
{
	return Impl->Progress;
}
auto FSceneImportPreview::GetResult() const -> const FAssetImportResult &
{
	return Impl->Result;
}
auto FSceneImportPreview::GetSourceMeshes() const -> std::vector<FAssetImportSourceMesh>
{
	std::vector<FAssetImportSourceMesh> Result;
	if (Impl->Progress.Phase == EAssetImportPhase::Ready)
		for (uint32 Index = 0; Index < Impl->Data.Document.Meshes.size(); ++Index)
		{
			const auto& Mesh = Impl->Data.Document.Meshes[Index];
			Result.push_back({Index, Mesh.SourceName, static_cast<uint32>(Mesh.Primitives.size()),
				std::ranges::contains(Impl->Data.SelectedMeshes, Index)});
		}
	return Result;
}
auto FSceneImportPreview::GetSourceScenes() const -> std::vector<FAssetImportSourceScene>
{
	std::vector<FAssetImportSourceScene> Result;
	if (Impl->Progress.Phase == EAssetImportPhase::Ready)
		for (uint32 Index = 0; Index < Impl->Data.Document.Scenes.size(); ++Index)
			Result.push_back({Index, Impl->Data.Document.Scenes[Index].SourceName,
				Index == Impl->Data.Document.DefaultSceneIndex});
	return Result;
}
auto FSceneImportPreview::SetOptions(FAssetImportOptions Options) -> bool
{
	if (Impl->Progress.Phase != EAssetImportPhase::Ready || Impl->bImportRequested || Impl->bTaskAccepted || Impl->bReplanRequested) return false;
	Impl->ImportOptions = std::move(Options);
	Impl->PreviewDescriptors.clear();
	Impl->PreviewSummaries.clear();
	Impl->bPlanResolved = false;
	Impl->bReplanRequested = true;
	Impl->Progress = {EAssetImportPhase::Reading, "Resolving selected resources"};
	return true;
}

auto FSceneImportPreview::PreviewOutputs(const FPackagePath& Destination,
	const FSceneMaterialImportOptions& Options) -> FAssetImportPreview
{ return ResolvePreview(Destination, Options, false); }

auto FSceneImportPreview::ResolvePreview(const FPackagePath& Destination,
	const FSceneMaterialImportOptions& Options, bool bAllowAccepted) -> FAssetImportPreview
{
	FAssetImportPreview Preview;
	if ((!bAllowAccepted && Impl->bTaskAccepted) || Impl->Progress.Phase != EAssetImportPhase::Ready || !Impl->Rebase(Destination))
	{
		Preview.Message = "Import preparation is not ready or destination is invalid.";
		return Preview;
	}
	auto Original = Impl->Data.Outputs;
	auto OriginalOptions = Impl->MaterialOptions;
	Impl->MaterialOptions = Options;
	FAssetImportResult Result;
	Result.Outputs = Impl->BaseOutputs;
	Preview.bSucceeded = Impl->ResolveOutputs(Result, Preview.Materials);
	Preview.Outputs = std::move(Result.Outputs);
	Preview.Diagnostics = std::move(Result.Diagnostics);
	for (const auto& Warning : Impl->Data.Warnings)
		Preview.Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Warning,
			.Category = EImportDiagnosticCategory::TranslationFailure, .Phase = "source", .SourceIdentity = "root", .Message = Warning});
	Preview.Message = std::move(Result.Message);
	Impl->PreviewDescriptors.clear();
	Impl->PreviewSummaries.clear();
	if (Preview.bSucceeded)
	{
		Impl->PreviewDescriptors = Impl->Data.Outputs;
		Impl->PreviewSummaries = Preview.Outputs;
		Impl->PreviewOptions = Options;
		Impl->PreviewDestination = Destination;
	}

	Impl->Data.Outputs = std::move(Original);
	Impl->MaterialOptions = std::move(OriginalOptions);
	return Preview;
}

auto FSceneImportPreview::BeginImport(const FPackagePath &Destination, FSceneMaterialImportOptions Options,
                                      std::optional<FAssetPackageSaveArgs> SaveArgs) -> bool
{
	if (Impl->Progress.Phase != EAssetImportPhase::Ready || Impl->bImportRequested || !Destination.IsValid())
		return false;
	if (!Impl->Rebase(Destination))
		return false;
	if (Impl->PreviewDescriptors.empty() || Impl->PreviewDestination != Destination || Impl->PreviewOptions != Options)
		if (!ResolvePreview(Destination, Options, true).bSucceeded) return false;
	Impl->Data.Outputs = Impl->PreviewDescriptors;
	Impl->BaseOutputs = Impl->PreviewSummaries;
	Impl->PreviewDescriptors.clear();
	Impl->PreviewSummaries.clear();
	Impl->bPlanResolved = true;
	Impl->MaterialOptions = std::move(Options);
	Impl->SaveArgs = std::move(SaveArgs);
	Impl->SaveFault = Testing::CapturePackageSaveFault();
	Impl->bImportRequested = true;
	return true;
}

auto FSceneImportPreview::ConfigureTask(DAssetImportTask& Task, const FPackagePath& Destination,
	FSceneMaterialImportOptions Options) -> bool
{
	if (!Task.IsAsyncImportComplete() || Impl->bTaskAccepted || Impl->Progress.Phase != EAssetImportPhase::Ready || Impl->bImportRequested || !Destination.IsValid()) return false;
	auto* Inputs = NewObject<DSceneImportOptions>(nullptr, "SceneTaskOptions", EObjectFlags::Transient);
	Inputs->Prepared = Impl; Inputs->MeshSettings = Impl->Settings; Inputs->Assets = Impl->ImportOptions;
	Inputs->Materials = std::move(Options);
	Task.Filename = Impl->SourceFile; Task.DestinationPath = Destination.ToString(); Task.Options = Inputs;
	Impl->TrackedTask = &Task;
	return true;
}

class FSceneTaskOperation final : public IAssetImportTaskOperation
{
public:
	explicit FSceneTaskOperation(const DAssetImportTask& Inputs)
	{
		FPackagePath Directory;
		if (!FPackagePath::TryCreate(Inputs.DestinationPath, Directory))
		{ FFeedbackContext::Get().Error("The scene destination is invalid."); return; }
		auto* Options = Cast<DSceneImportOptions>(Inputs.Options.Get());
		if (Inputs.Options && !Options)
		{ FFeedbackContext::Get().Error("Scene import requires scene import options."); return; }
		if (Options && Options->Prepared)
		{
			if (Options->Prepared->bImportRequested || Options->Prepared->SourceFile != Inputs.Filename)
			{ FFeedbackContext::Get().Error("The scene preview is already submitted or belongs to another source."); return; }
			if (Options->MeshSettings != Options->Prepared->Settings || Options->Assets != Options->Prepared->ImportOptions)
			{ FFeedbackContext::Get().Error("Prepared scene settings changed; refresh the preview before importing."); return; }
			Preview.reset(new FSceneImportPreview(std::exchange(Options->Prepared, {})));
		}
		else Preview = std::make_unique<FSceneImportPreview>(Inputs.Filename, Directory,
			Options ? Options->MeshSettings : FStaticMeshImportSettings::MakeYUpNegativeZForward(),
			Options ? Options->Assets : FAssetImportOptions{});
		Preview->Impl->bTaskAccepted = true;
		Destination = Directory; Name = Inputs.DestinationName;
		Materials = Options ? Options->Materials : FSceneMaterialImportOptions{};
		if (Inputs.bSave) SaveArgs.emplace();
		SaveFault = Testing::CapturePackageSaveFault();
		// Models remain one-time imports. An occupied scene output is explicitly
		// unsupported replacement; standalone family task replacement is separate.
	}
	~FSceneTaskOperation() override
	{ if (Preview) { Preview->Impl->OutputOwners.clear(); Preview->Impl->OutputObjects.clear(); } }
	auto Tick() -> void override
	{
		if (!Preview || Complete) return;
		PumpGameThreadDeferredWork();
		FAssetCompilingManager::Get().ProcessAsyncTasks(false);
		auto& State = *Preview->Impl;
		State.Tick();
		if (State.Progress.Phase == EAssetImportPhase::Ready && !State.bImportRequested)
		{
			if (!State.Rebase(Destination)) State.Cancellation.Cancel();
			if (!Name.empty())
			{
				std::vector<FImportOutputSummary*> Meshes;
				for (auto& Output : State.BaseOutputs) if (Output.Role == "StaticMesh") Meshes.push_back(&Output);
				FPackagePath Path;
				FTopLevelAssetPath Identity;
				if (Meshes.size() != 1 || !FPackagePath::TryCreate(Destination.ToString() + "/Meshes/" + Name, Path)
					|| !FTopLevelAssetPath::TryCreate(Path, Name, Identity))
				{
					auto& Rejection = State.Rejection.emplace();
					Rejection.Message = "An explicit scene destination name requires one mesh and a valid name.";
					Rejection.Diagnostics.push_back({.Category = EImportDiagnosticCategory::InvalidRequest, .Message = Rejection.Message});
					State.Cancellation.Cancel();
				}
				else { Meshes.front()->AssetPath = Path; State.PreviewDescriptors.clear(); }
				Name.clear();
			}
			if (!State.CheckCancellation())
			{
				const auto Plan = Preview->ResolvePreview(Destination, Materials, true);
				if (!Plan.bSucceeded)
				{
					auto& Rejection = State.Rejection.emplace();
					Rejection.Outputs = Plan.Outputs; Rejection.Diagnostics = Plan.Diagnostics; Rejection.Message = Plan.Message;
					State.Cancellation.Cancel();
				}
				else if (!Preview->BeginImport(Destination, Materials, SaveArgs)) State.Cancellation.Cancel();
				else State.SaveFault = SaveFault;
			}
		}
		if (State.Progress.Phase == EAssetImportPhase::Completed)
		{
			Complete = true;
			if (!State.Result.Message.empty()) FFeedbackContext::Get().Error(State.Result.Message);
		}
	}
	auto Cancel() -> void override { if (Preview) Preview->Cancel(); }
	auto IsComplete() const -> bool override { return !Preview || Complete; }
	auto GetObjects() const -> std::span<DObject* const> override
	{ return Preview ? std::span<DObject* const>(Preview->Impl->OutputObjects) : std::span<DObject* const>{}; }
private:
	std::unique_ptr<FSceneImportPreview> Preview;
	FPackagePath Destination;
	std::string Name;
	FSceneMaterialImportOptions Materials;
	std::optional<FAssetPackageSaveArgs> SaveArgs;
	Testing::FPackageSaveFault SaveFault;
	bool Complete = false;
};

auto MakeSceneImportAdapter() -> std::unique_ptr<FAssetImportAdapterRegistration>
{
	return std::make_unique<FAssetImportAdapterRegistration>(
		[](const DAssetImportTask& Task, bool Scene) {
			return !Task.Factory && IsOneTimeModelSource(Task.Filename)
				&& (Scene || !Task.Options || Cast<DSceneImportOptions>(Task.Options.Get()));
		},
		[](const DAssetImportTask& Inputs) { return std::make_shared<FSceneTaskOperation>(Inputs); });
}

} // namespace Durin::AssetForge::Builtins
