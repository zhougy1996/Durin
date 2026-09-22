#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "AssetForge/Builtins/SceneImport.h"
#include "AssetForge/Builtins/SceneImportData.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Threading/TaskComposition.h"
#include <coroutine>
#include <thread>

#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/AssetImportData.h"
#include "Asset/PackageSerialization.h"
#include "Asset/SourceHint.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "SceneImportInternal.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuilder.h"
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
	std::unique_ptr<FStaticMeshAuthoredCandidate> StaticMesh;
	FSceneTextureBuildProduct Texture;
	DObject *Candidate = nullptr;
	DPackage *Package = nullptr;
	DObject *Previous = nullptr;
	TStrongObjectPtr<DObject> CandidatePin;
	TStrongObjectPtr<DObject> PreviousPin;
	uint64 PreviousRevision = 0;
};

auto GetSceneOutputIdentity(DObject *Object, std::string_view Source) -> std::string
{
	if (const auto *Material = Cast<DMaterialInterface>(Object))
	{
		const auto &Receipt = Material->GetImportProvenance();
		if (Receipt.RecipeId == "Durin.ImportedSurface" && Receipt.RecipeVersion == 1 &&
		    Receipt.SourceIdentity == Source)
			return Receipt.OutputIdentity;
	}
	const DAssetImportData *Data = nullptr;
	if (const auto *Texture = Cast<DTexture2D>(Object))
		Data = Texture->GetAssetImportData();
	if (const auto *Mesh = Cast<DStaticMesh>(Object))
		Data = Mesh->GetAssetImportData();
	const auto *Receipt = Cast<DSceneImportData>(Data);
	return Receipt && Receipt->SourceIdentity == Source ? Receipt->OutputIdentity : std::string{};
}

auto AddError(FSceneImportResult &Result, EImportDiagnosticCategory Category, std::string Phase,
              std::string Message, std::string OutputIdentity = {}) -> FSceneImportResult
{
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

auto MakeStableOutputOrder(const FSceneImportPlan &Data, std::vector<size_t> &OutOrder, std::string &OutError)
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
	std::vector<std::string> MaterialIdentities;
	for (const FSceneOutputData &Output : Data.Outputs)
		if (IsSceneMaterial(Output.Kind))
			MaterialIdentities.push_back(Output.StableIdentity);
	for (size_t Index = 0; Index < Data.Outputs.size(); ++Index)
	{
		const FSceneOutputData &Output = Data.Outputs[Index];
		std::vector<std::string> Dependencies;
		if (IsSceneMaterial(Output.Kind))
			for (const FSceneMaterialTextureBinding &Binding : Output.TextureBindings)
				Dependencies.push_back(Binding.TextureIdentity);
		if (Output.Kind == ESceneOutputKind::StaticMesh)
			Dependencies.insert(Dependencies.end(), MaterialIdentities.begin(), MaterialIdentities.end());
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
	FAssetCompilingManager::Get().FinishCompilationForObjects(Objects);
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
	DPackage *Package = NewObject<DPackage>(nullptr, FName(AssetPath.GetPackagePath().GetPackageName()),
	                                        EObjectFlags::Standalone);
	if (!Package || !Package->InitializePreparedAssetPackage(AssetPath.GetPackagePath()))
	{
		OutError = "The scene candidate package could not be created.";
		return false;
	}
	FStaticConstructObjectParameters Parameters{T::StaticClass(), Package, FName(AssetPath.GetAssetName()),
	                                            sizeof(T), EObjectFlags::Public};
	DObject *Object = StaticConstructObject(Parameters);
	DObjectForceRegistration(Object);
	OutAsset = Cast<T>(Object);
	if (!OutAsset || Package->FindTopLevelAsset(OutAsset->GetFName()) != OutAsset)
	{
		MarkObjectHierarchyAsGarbage(Package);
		CollectGarbage();
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
		FSceneImportResult Result;
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
		auto return_value(FSceneImportResult Value) -> void
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

struct FSceneImportSession::FImpl
{
	std::string SourceFile;
	std::string RootFilename;
	FPackagePath DestinationDirectory;
	FStaticMeshImportSettings Settings;
	FSceneMaterialImportOptions MaterialOptions;
	FSceneImportPublicationOptions PublicationOptions;
	bool bAsync = true;
	bool bImportRequested = false;
	std::atomic<bool> Canceled = false;
	std::function<bool()> IsCancellationRequested;
	FSceneImportProgress Progress;
	FSceneImportResult Result;
	std::shared_ptr<const FSourceSnapshot> Snapshot;
	FSceneImportPlan Data;
	std::vector<FImportOutputSummary> BaseOutputs;
	std::vector<FPreparedSceneOutput> Prepared;
	std::function<bool()> ReadyToResume;
	Tasks::FTask Worker;
	std::optional<FSceneRoutine> Routine;
	bool bCleaning = false;
	bool bWorkerFailed = false;
	std::atomic<size_t> BuiltCount = 0;

	struct FStep
	{
		FImpl &Owner;
		std::function<bool()> Ready;
		auto await_ready() const noexcept -> bool
		{
			return !Owner.bAsync;
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
		if (!bAsync)
		{
			Body();
			return Step();
		}
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
			    Material && Material->GetMaterialCompileStatus().State == EMaterialCompileState::Scheduled)
				RequestMaterialRecompile(*Material);
		if (!bAsync)
			FAssetCompilingManager::Get().FinishCompilationForObjects(Objects);
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
	auto Tick() -> void;
	auto Run() -> FSceneRoutine;
	auto PrepareSource(FSceneImportResult &Result) -> void;
	auto BuildProducts(FSceneImportResult &Result) -> void;
};

auto FSceneImportSession::FImpl::PrepareSource(FSceneImportResult &Result) -> void
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
	                    Result.Message))
	{
		if (!Result.Diagnostics.empty() && !Result.Diagnostics.back().Message.empty())
			Result.Message = Result.Diagnostics.back().Message;
		return;
	}
}

auto FSceneImportSession::FImpl::BuildProducts(FSceneImportResult &Result) -> void
{
	Private::FScopedSceneImportCancellation CancellationScope(IsCancellationRequested);
	// Validate the captured closure without parsing the source a second time.
	if (bAsync)
		for (const auto &Source : Snapshot->GetSources())
		{
			if (IsCanceled(IsCancellationRequested))
			{
				Result.Message = "Scene import canceled.";
				return;
			}
			const auto Hash = FFileHelper::HashFileXx128(Source.Filename);
			if (!Hash || *Hash != Source.ContentHash)
			{
				Result.Message =
				    "Scene source changed after preparation. Select the source again to refresh.";
				return;
			}
		}
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
			if (const auto Initialized =
			        Output.StaticMeshSource.Initialize(MakeStaticMeshDecodedGeometry(Data.Scene));
			    !Initialized)
			{
				Result = AddError(Result, EImportDiagnosticCategory::CandidateFailure, "scene-build",
				                  FormatStaticMeshSourceError(Initialized.error()), Descriptor.StableIdentity);
				return;
			}
			auto Outcome =
			    FStaticMeshBuilder::BuildCandidate({.Source = Output.StaticMeshSource},
			                                     {.ShouldCancel = IsCancellationRequested});
			if (!Outcome)
			{
				Result = AddError(Result,
				                  (!Outcome && Outcome.error().Code == EStaticMeshAuthoredBuildError::Cancelled)
				                      ? EImportDiagnosticCategory::Canceled
				                      : EImportDiagnosticCategory::CandidateFailure,
				                  "scene-build", Outcome.error().ToString(),
				                  Descriptor.StableIdentity);
				return;
			}
			Output.StaticMesh = std::move(*Outcome);
		}
		++BuiltCount;
	}
}

auto FSceneImportSession::FImpl::Run() -> FSceneRoutine
{
	FSceneImportResult Result;
	try
	{
		if (IsCanceled(IsCancellationRequested))
			co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-request",
			                   "Scene import canceled.");
		if (SourceFile.empty() || !DestinationDirectory.IsValid() || !Settings.Validate())
			co_return AddError(Result, EImportDiagnosticCategory::InvalidRequest, "scene-request",
			                   "Scene import request is invalid.");
		Progress = {ESceneImportPhase::Reading, "Reading scene and dependencies"};
		co_await Work([&] { PrepareSource(Result); });
		if (IsCanceled(IsCancellationRequested))
			co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-prepare",
			                   "Scene import canceled.");
		if (!Result.Message.empty() || !Snapshot)
			co_return std::move(Result);
		BaseOutputs = Result.Outputs;
		if (bAsync)
		{
			Progress = {ESceneImportPhase::Ready, "Configure materials"};
			co_await Step([this] { return bImportRequested || Canceled.load(); });
			if (Canceled.load())
				co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-preview",
				                   "Scene import canceled.");
		}
		Result.Outputs = BaseOutputs;
		Progress = {ESceneImportPhase::Building, "Building textures and mesh", 0, Data.Outputs.size()};
		co_await Work([&] { BuildProducts(Result); });
		if (IsCanceled(IsCancellationRequested))
			co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-build",
			                   "Scene import canceled.");
		if (!Result.Message.empty())
			co_return std::move(Result);
		std::vector<FSceneMaterialPreview> MaterialPreview;
		if (!ConfigureSceneMaterials(Data, Result.Outputs, RootFilename, DestinationDirectory,
		                             MaterialOptions, MaterialPreview, Result.Message))
			co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure, "scene-material-policy",
			                   Result.Message);
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
		Progress = {ESceneImportPhase::Preparing, "Preparing assets", 0, Prepared.size()};
		// Match receipts in the destination before considering generated filenames.
		// An unrelated asset at a requested path is never replacement authority.
		std::unordered_map<std::string, DObject *> ExistingOutputs;
		std::vector<TStrongObjectPtr<DObject>> ExistingPins;
		std::vector<FPackagePath> ExistingPaths;
		const std::string Prefix = DestinationDirectory.ToString() + "/";
		for (const auto &[Path, Entry] : CaptureAssetCatalogSnapshot().Assets)
			if (Path.GetView().starts_with(Prefix))
				ExistingPaths.push_back(Path);
		for (auto *Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
			if (auto *Package = Cast<DPackage>(Object);
			    Package && Package->GetPackagePath().starts_with(Prefix) &&
			    !std::ranges::contains(ExistingPaths, Package->GetPackagePathIdentity()))
				ExistingPaths.push_back(Package->GetPackagePathIdentity());
		for (const auto &Path : ExistingPaths)
		{
			co_await Step();
			DObject *Object = nullptr;
			FObjectPath ObjectPath;
			if (!FObjectPath::TryCreate(Path.ToString() + "." + std::string(Path.GetPackageName()),
			                            ObjectPath) ||
			    !(Object = LoadObject<DObject>(ObjectPath).value_or(nullptr)))
				continue;
			ExistingPins.emplace_back(Object);
			const auto Identity = GetSceneOutputIdentity(Object, RootFilename);
			if (!Identity.empty() && !ExistingOutputs.emplace(Identity, Object).second)
				co_return AddError(Result, EImportDiagnosticCategory::Collision, "scene-publication",
				                   "Multiple saved outputs claim the same scene identity.", Identity);
		}
		for (FPreparedSceneOutput &Output : Prepared)
		{
			Progress.Activity = Output.AssetPath.ToString();
			Progress.Completed = static_cast<size_t>(&Output - Prepared.data());
			co_await Step();
			if (IsCanceled(IsCancellationRequested))
				co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-prepare",
				                   "Scene import canceled.");
			const auto Existing = ExistingOutputs.find(Output.Descriptor->StableIdentity);
			if (Existing != ExistingOutputs.end())
			{
				Output.Previous = Existing->second;
				Output.PreviousPin = TStrongObjectPtr<DObject>(Output.Previous);
				Output.PreviousRevision = Output.Previous->GetPackage()->GetEditRevision();
				Output.AssetPath = Output.Previous->GetPackage()->GetPackagePathIdentity();
				const bool bTypeMatches = Output.Descriptor->Kind == ESceneOutputKind::MaterialInstance
				                              ? Cast<DMaterialInstance>(Output.Previous) != nullptr
				                          : Output.Descriptor->Kind == ESceneOutputKind::Material
				                              ? Cast<DMaterial>(Output.Previous) != nullptr
				                          : Output.Descriptor->Kind == ESceneOutputKind::StaticMesh
				                              ? Cast<DStaticMesh>(Output.Previous) != nullptr
				                              : Cast<DTexture2D>(Output.Previous) != nullptr;
				if (!bTypeMatches || Output.Previous->GetPackage()->GetTopLevelAssets().size() != 1)
					co_return AddError(Result, EImportDiagnosticCategory::Collision, "scene-publication",
					                   "The previous scene output has an incompatible package shape.",
					                   Output.Descriptor->StableIdentity);
			}
			else if (FindAssetExact(Output.AssetPath) || FindResidentPackage(Output.AssetPath))
			{
				// A changed derivation gets a distinct output; keep the old asset for
				// existing references instead of overwriting its identity.
				const auto Occupant = std::ranges::find_if(
				    ExistingOutputs, [&](const auto &Entry)
				    { return Entry.second->GetPackage()->GetPackagePathIdentity() == Output.AssetPath; });
				if (Occupant == ExistingOutputs.end() ||
				    Output.Descriptor->Kind != ESceneOutputKind::Texture2D ||
				    !FPackagePath::TryCreate(
				        Output.AssetPath.ToString() + "_" +
				            FXxHash128::HashBuffer(
				                std::as_bytes(std::span(Output.Descriptor->StableIdentity)))
				                .ToString(),
				        Output.AssetPath) ||
				    FindAssetExact(Output.AssetPath) || FindResidentPackage(Output.AssetPath))
					co_return AddError(Result, EImportDiagnosticCategory::Collision, "scene-publication",
					                   "Scene output path is occupied by an unrelated output.",
					                   Output.Descriptor->StableIdentity);
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
				Output.Candidate = Output.Previous;
				Output.Package = Output.Previous->GetPackage();
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
					    (PackageResolution ? std::string{} : Durin::ToString(PackageResolution.error())),
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
				FTexture2DBuildProduct &Product = Output.Texture.Product;
				const FTexture2DBuildSettings &Settings = Output.Texture.Settings;
				auto PlatformData = std::make_unique<FTexturePlatformData>(std::move(Product.PlatformData));
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
				if (const auto Applied = FStaticMeshBuilder::ApplyCandidate(
				        *Mesh, std::move(Output.StaticMesh), FStaticMeshBuilder::Capture(*Mesh), true, {},
				        ImportData);
				    !Applied)
				{
					co_return AddError(
					    Result, EImportDiagnosticCategory::CandidateFailure, "scene-materialization",
					    FormatStaticMeshApplicationError(Applied.error()), Descriptor.StableIdentity);
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
				if (Descriptor.bStandardPBRParent)
				{
					Recipe.Owners.clear();
					for (uint32 Role = 0; Role < Roles.size(); ++Role)
						for (uint32 Kind = 0; Kind < MaterialParameters::BuiltinParameterKindCount; ++Kind)
						{
							const auto Surface = static_cast<EMaterialSurfaceOutput>(Role);
							const auto ParameterKind =
							    static_cast<MaterialParameters::EMaterialBuiltinParameterKind>(Kind);
							Recipe.Owners.push_back({Surface, ParameterKind,
							                         GetMaterialSurfaceParameterId(Surface, ParameterKind)});
						}
				}
				auto *Standard = Descriptor.Kind == ESceneOutputKind::Material
				                     ? Cast<DMaterial>(Output.Candidate)
				                     : Descriptor.Parent.Get();
				if (Descriptor.Kind == ESceneOutputKind::Material && Standard)
				{
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
					const auto *Definition = Standard->FindParameterDefinition(Owner.ParameterId);
					require(Definition);
					const auto &Role = Roles[static_cast<uint32>(Owner.Role)];
					const auto *Sample = Role.Sample ? &*Role.Sample : nullptr;
					// The full template reads opacity from alpha, while compact imports use
					// the derived red-channel mask. Alpha is unaffected by the source's sRGB view.
					if (Descriptor.bStandardPBRParent && Owner.Role == EMaterialSurfaceOutput::Opacity &&
					    Sample)
						Sample = &*Roles[0].Sample;
					const auto StandardDefinitions = GetPBRMaterialParameterDefinitions();
					const auto StandardDefinition = std::ranges::find(StandardDefinitions, Owner.ParameterId,
					                                                  &FMaterialParameterDefinition::Id);
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
						                   Owner.Role == EMaterialSurfaceOutput::Emissive && Sample
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
					if (const auto Applied = Instance ? Instance->SetParameterValue(Owner.ParameterId, Value)
					                                  : Standard->SetParameterValue(Owner.ParameterId, Value);
					    !Applied)
					{
						co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure,
						                   "scene-material-parameters", FormatMaterialError(Applied.Error),
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
					if (IsSceneMaterial(Candidate.Kind))
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
						if (!MaterialOptions.bRebuildExistingMaterials)
							if (auto *PreviousMesh = Cast<DStaticMesh>(Output.Previous))
							{
								const auto &Slot = Mesh->GetMaterialSlots()[SlotIndex];
								const auto Slots = PreviousMesh->GetMaterialSlots();
								const bool bUniqueName =
								    !Slot.SourceName.empty() &&
								    std::ranges::count(Slots, Slot.SourceName,
								                       &FMeshMaterialSlotDefinition::SourceName) == 1 &&
								    std::ranges::count(Mesh->GetMaterialSlots(), Slot.SourceName,
								                       &FMeshMaterialSlotDefinition::SourceName) == 1;
								const auto PreviousSlot = std::ranges::find_if(
								    Slots,
								    [&](const auto &Old)
								    {
									    return Old.SourceName == Slot.SourceName &&
									           (bUniqueName ||
									            Old.SourceMaterialIndex == Slot.SourceMaterialIndex);
								    });
								if (PreviousSlot != Slots.end())
									Mesh->SetMaterialSlotDefaultMaterial(SlotIndex,
									                                     PreviousSlot->DefaultMaterial.Get());
							}
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

		Progress = {ESceneImportPhase::Compiling, "Compiling materials"};
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
			        : Durin::FormatMaterialError(Diagnostics.front().Source.Error);
			co_return AddError(Result, EImportDiagnosticCategory::ValidationFailure, "scene-material-compile",
			                   Message);
		}

		if (IsCanceled(IsCancellationRequested))
		{
			co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-publication",
			                   "Scene import was canceled before persistence.");
		}
		ExistingPins.clear();
		std::vector<FObjectReplacementPackagePair> Pairs;
		for (const auto &Output : Prepared)
			if (!Output.Descriptor->PreservedMaterial.Get())
				Pairs.push_back({Output.Previous ? Output.Previous->GetPackage() : nullptr, Output.Package});
		for (size_t Index = 0; Index < Pairs.size(); ++Index)
		{
			const auto &Pair = Pairs[Index];
			std::vector<DObject *> ExternalConsumers;
			std::vector<TStrongObjectPtr<DObject>> ConsumerPins;
			std::vector<DObject *> DependentMaterials;
			for (auto *Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
				if (Object->GetPackage() != Pair.Current)
				{
					ExternalConsumers.push_back(Object);
					ConsumerPins.emplace_back(Object);
					if (auto *Material = Cast<DMaterialInstance>(Object))
						for (auto *Parent = Material->GetParent(); Parent; Parent = Parent->GetParent())
							if (Pair.Current && Parent->GetPackage() == Pair.Current)
							{
								DependentMaterials.push_back(Material);
								break;
							}
				}
			auto PreviousCompilations = DependentMaterials;
			for (const auto &Output : Prepared)
				if (Output.Previous && Output.Previous->GetPackage() == Pair.Current)
					PreviousCompilations.push_back(Output.Previous);
			co_await Compilation(PreviousCompilations);

			FObjectGraphReplacement Publication;
			FAssetWriteResult PersistenceResult;
			FAssetBundleSaveOptions SaveOptions{
			    .RootPackage = Index + 1 == Pairs.size() ? Pair.Prepared : nullptr,
			    .ShouldFail = [this, Index](EAssetBundleSavePhase Phase, size_t)
			    { return PublicationOptions.ShouldFail && PublicationOptions.ShouldFail(Phase, Index); },
			    .bRollbackOnRegistryFailure = true};
			std::shared_ptr<FPreparedAssetSave> StagedSave;
			if (bAsync)
			{
				Progress = {ESceneImportPhase::Saving, Pair.Prepared->GetPackagePath(), Index, Pairs.size()};
				StagedSave = FPreparedAssetSave::Begin(Pair.Prepared, SaveOptions, PersistenceResult);
				if (StagedSave)
					co_await Step([&] { return StagedSave->IsReady(); });
				if (IsCanceled(IsCancellationRequested))
					co_return AddError(Result, EImportDiagnosticCategory::Canceled, "scene-save",
					                   "Scene import canceled before package commit.");
				if (!StagedSave)
					co_return AddError(Result, EImportDiagnosticCategory::PersistenceFailure, "scene-save",
					                   PersistenceResult.Message);
			}
			// Staging yields before graph preparation: unrelated object creation during
			// disk I/O must not invalidate a reference-replacement snapshot.
			for (auto &Output : Prepared)
				if (Output.Package == Pair.Prepared && Output.Previous)
				{
					if (Output.Previous->GetPackage()->GetEditRevision() != Output.PreviousRevision ||
					    FindResidentPackage(Output.AssetPath) != Output.Previous->GetPackage())
						co_return AddError(Result, EImportDiagnosticCategory::Collision, "scene-publication",
						                   "An existing output changed during import; retry.");
					Output.PreviousPin.Reset();
				}
			const std::array Participants{MakeMaterialReferenceReplacementParticipant()};
			auto Published = Publication.Prepare(std::span(&Pair, 1), Participants);
			if (Published)
			{
				SaveOptions.PreparedPublication = &Publication;
				Published = Publication.TryCommit(
				    [&]() -> std::expected<void, FObjectReplacementError>
				    {
					    PersistenceResult =
					        bAsync ? StagedSave->Commit(Publication)
					               : SavePackages(std::span(&Pair.Prepared, 1), SaveOptions).Result;
					    if (!PersistenceResult)
						    return std::unexpected(FObjectReplacementError{
						        .Code = EObjectReplacementError::ParticipantRejected,
						        .Reason = EObjectReplacementReason::PersistenceRejected});
					    return {};
				    });
			}
			StagedSave.reset();
			if (!Published)
			{
				Publication.Abort();
				co_return AddError(Result, EImportDiagnosticCategory::PersistenceFailure, "scene-persistence",
				                   std::format("Saved {} of {} packages; {}: {}", Result.SavedPackages.size(),
				                               Pairs.size(), Pair.Prepared->GetPackagePath(),
				                               !PersistenceResult ? PersistenceResult.Message
				                                                  : ToString(Published.error())));
			}
			Pair.Prepared->MarkAsPublished();
			Result.SavedPackages.push_back(Pair.Prepared->GetPackagePathIdentity());
			if (Pair.Current)
			{
				// Refresh consumers after each publication, including on partial imports.
				for (auto *Object : DependentMaterials)
					RequestMaterialRecompile(*Cast<DMaterialInterface>(Object));
				co_await Compilation(DependentMaterials);
				for (auto *Object : ExternalConsumers)
				{
					if (auto *Material = Cast<DMaterialInterface>(Object))
						Material->RefreshReloadedAssetBindings();
					if (auto *Cloud = Cast<DVolumetricCloudComponent>(Object))
						Cloud->RefreshReloadedAssetBindings();
					if (auto *Sky = Cast<DSkyLightComponent>(Object))
						Sky->RefreshReloadedAssetBindings();
					if (auto *Mesh = Cast<DStaticMeshComponent>(Object))
						Mesh->RefreshReloadedAssetBindings();
					else if (auto *Primitive = Cast<DPrimitiveComponent>(Object))
						Primitive->MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
				}
				FRenderCommandFence Fence;
				Fence.BeginFence(ERenderCommandFenceMode::RHIThread);
				if (bAsync)
					co_await Step([&] { return Fence.IsFenceComplete(); });
				else
					Fence.Wait();
			}
			if (bAsync)
				co_await Step([&] { return Publication.Retire(); });
			else
				require(Publication.Retire());
		}

		Result.bSucceeded = true;
		Result.bPersisted = true;
		Result.Message.clear();
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

auto FSceneImportSession::FImpl::Tick() -> void
{
	Progress.bCancellationRequested = Canceled.load();
	if (Progress.Phase == ESceneImportPhase::Building)
		Progress.Completed = BuiltCount.load();
	if (Progress.Phase == ESceneImportPhase::Completed)
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
		bCleaning = true;
		Progress = {ESceneImportPhase::Finishing, "Releasing import resources"};
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
	if (bAsync && MaterialWorkPending(Objects))
		return;
	Abandon(Prepared);
	Prepared.clear();
	Data = {};
	Snapshot.reset();
	Routine.reset();
	Progress = {ESceneImportPhase::Completed, Result.bSucceeded ? "Import complete" : Result.Message};
}

FSceneImportSession::FSceneImportSession(std::string SourceFile, FPackagePath Destination,
                                         FStaticMeshImportSettings Settings)
    : Impl(std::make_unique<FImpl>())
{
	Impl->SourceFile = std::move(SourceFile);
	Impl->DestinationDirectory = std::move(Destination);
	Impl->Settings = std::move(Settings);
	Impl->IsCancellationRequested = [State = Impl.get()] { return State->Canceled.load(); };
}
FSceneImportSession::~FSceneImportSession()
{
	Cancel();
	// Only host teardown drains synchronously; normal UI cancellation keeps ticking.
	while (Impl->Progress.Phase != ESceneImportPhase::Completed)
	{
		PumpGameThreadDeferredWork();
		FAssetCompilingManager::Get().ProcessAsyncTasks(false);
		Impl->Tick();
		if (Impl->Progress.Phase != ESceneImportPhase::Completed)
			std::this_thread::yield();
	}
}
auto FSceneImportSession::Tick() -> void
{
	Impl->Tick();
}
auto FSceneImportSession::Cancel() -> void
{
	Impl->Canceled.store(true);
	Impl->Progress.bCancellationRequested = true;
}
auto FSceneImportSession::GetProgress() const -> const FSceneImportProgress &
{
	return Impl->Progress;
}
auto FSceneImportSession::GetResult() const -> const FSceneImportResult &
{
	return Impl->Result;
}
auto FSceneImportSession::PreviewMaterials(const FPackagePath &Destination,
                                           const FSceneMaterialImportOptions &Options)
    -> FSceneMaterialPreviewResult
{
	FSceneMaterialPreviewResult Result;
	if (Impl->Progress.Phase != ESceneImportPhase::Ready || !Destination.IsValid())
	{
		Result.Message = "Scene preparation is not ready.";
		return Result;
	}
	if (!Impl->Rebase(Destination))
	{
		Result.Message = "The generated scene output path is invalid.";
		return Result;
	}
	auto Original = Impl->Data.Outputs;
	auto Outputs = Impl->BaseOutputs;
	Result.bSucceeded = ConfigureSceneMaterials(Impl->Data, Outputs, Impl->RootFilename, Destination, Options,
	                                            Result.Materials, Result.Message);
	Impl->Data.Outputs = std::move(Original);
	return Result;
}
auto FSceneImportSession::BeginImport(const FPackagePath &Destination, FSceneMaterialImportOptions Options,
                                      FSceneImportPublicationOptions PublicationOptions) -> bool
{
	if (Impl->Progress.Phase != ESceneImportPhase::Ready || Impl->bImportRequested || !Destination.IsValid())
		return false;
	if (!Impl->Rebase(Destination))
		return false;
	Impl->MaterialOptions = std::move(Options);
	Impl->PublicationOptions = std::move(PublicationOptions);
	Impl->bImportRequested = true;
	return true;
}

auto ImportSceneAssets(std::string_view SourceFile, const FPackagePath &DestinationDirectory,
                       const FStaticMeshImportSettings &Settings,
                       const std::function<bool()> &IsCancellationRequested,
                       const FSceneImportPublicationOptions &PublicationOptions,
                       const FSceneMaterialImportOptions &MaterialOptions) -> FSceneImportResult
{
	FSceneImportSession Session(std::string(SourceFile), DestinationDirectory, Settings);
	Session.Impl->bAsync = false;
	Session.Impl->bImportRequested = true;
	Session.Impl->MaterialOptions = MaterialOptions;
	Session.Impl->PublicationOptions = PublicationOptions;
	Session.Impl->IsCancellationRequested = IsCancellationRequested;
	Session.Tick();
	return Session.GetResult();
}
} // namespace Durin::AssetForge::Builtins
