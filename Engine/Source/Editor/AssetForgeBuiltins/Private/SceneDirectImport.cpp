#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "AssetForge/Builtins/SceneImport.h"
#include "AssetForge/Builtins/SceneImportData.h"
#include "Hash/XxHash.h"

#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/SourceHint.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Package.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/DObjectArray.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "RenderingThread.h"
#include "Asset/AssetImportData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "SceneImportInternal.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMeshImportAdapter.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DCompilation.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		auto ResolveImportedMaterialSlot(const DStaticMesh& Mesh, uint32 SourceMaterialIndex,
			uint32& OutSlotIndex, std::string& OutError) -> bool
		{
			const auto Slots = Mesh.GetMaterialSlots();
			const auto Slot = std::ranges::find(Slots, SourceMaterialIndex,
				&FMeshMaterialSlotDefinition::SourceMaterialIndex);
			if (Slot == Slots.end())
			{
				OutError = std::format("Static mesh has no slot for source material {}.", SourceMaterialIndex);
				return false;
			}
			if (std::find_if(std::next(Slot), Slots.end(), [&](const auto& Other) {
				return Other.SourceMaterialIndex == SourceMaterialIndex;
			}) != Slots.end())
			{
				OutError = std::format("Static mesh has ambiguous slots for source material {}.", SourceMaterialIndex);
				return false;
			}
			OutSlotIndex = static_cast<uint32>(std::distance(Slots.begin(), Slot));
			OutError.clear();
			return true;
		}

		auto GetScenePublicationMutex() -> std::mutex&
		{
			static std::mutex Mutex;
			return Mutex;
		}

		struct FPreparedSceneOutput
		{
			const FSceneOutputData* Descriptor = nullptr;
			FPackagePath AssetPath;
			FStaticMeshSource StaticMeshSource;
			std::unique_ptr<FStaticMeshAuthoredCandidate> StaticMesh;
			FSceneTextureBuildProduct Texture;
			DObject* Candidate = nullptr;
			DPackage* Package = nullptr;
			DObject* Previous = nullptr;
		};

		auto GetSceneOutputIdentity(DObject* Object, std::string_view Source) -> std::string
		{
			if (const auto* Material = Cast<DMaterialInstance>(Object))
			{
				const auto& Receipt = Material->GetImportProvenance();
				if (Receipt.RecipeId == "Durin.ImportedSurface" && Receipt.RecipeVersion == 1 &&
					Receipt.SourceIdentity == Source) return Receipt.OutputIdentity;
			}
			const DAssetImportData* Data = nullptr;
			if (const auto* Texture = Cast<DTexture2D>(Object)) Data = Texture->GetAssetImportData();
			if (const auto* Mesh = Cast<DStaticMesh>(Object)) Data = Mesh->GetAssetImportData();
			const auto* Receipt = Cast<DSceneImportData>(Data);
			return Receipt && Receipt->SourceIdentity == Source ? Receipt->OutputIdentity : std::string{};
		}

		struct FGeneratedParentScope
		{
			std::vector<DPackage*> Packages;
			bool bRetain = false;
			~FGeneratedParentScope()
			{
				if (!bRetain)
					for (auto It = Packages.rbegin(); It != Packages.rend(); ++It)
						if (IsValid(*It)) MarkObjectHierarchyAsGarbage(*It);
			}
		};

		auto AddError(FSceneImportResult& Result, EImportDiagnosticCategory Category,
			std::string Phase, std::string Message,
			std::string OutputIdentity = {}) -> bool
		{
			Result.Diagnostics.push_back({
				.Severity = EImportDiagnosticSeverity::Error,
				.Category = Category,
				.Phase = std::move(Phase),
				.OutputIdentity = std::move(OutputIdentity),
				.Message = Message});
			Result.Message = std::move(Message);
			return false;
		}

		auto IsCanceled(const std::function<bool()>& Predicate) -> bool
		{
			return Predicate && Predicate();
		}

		auto MakeStableOutputOrder(
			const FSceneImportPlan& Data,
			std::vector<size_t>& OutOrder,
			std::string& OutError) -> bool
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
			for (const FSceneOutputData& Output : Data.Outputs)
				if (Output.Kind == ESceneOutputKind::MaterialInstance)
					MaterialIdentities.push_back(Output.StableIdentity);
			for (size_t Index = 0; Index < Data.Outputs.size(); ++Index)
			{
				const FSceneOutputData& Output = Data.Outputs[Index];
				std::vector<std::string> Dependencies;
				if (Output.Kind == ESceneOutputKind::MaterialInstance)
					for (const FSceneMaterialTextureBinding& Binding : Output.TextureBindings)
						Dependencies.push_back(Binding.TextureIdentity);
				if (Output.Kind == ESceneOutputKind::StaticMesh)
					Dependencies.insert(Dependencies.end(),
						MaterialIdentities.begin(), MaterialIdentities.end());
				std::ranges::sort(Dependencies);
				Dependencies.erase(std::unique(Dependencies.begin(), Dependencies.end()),
					Dependencies.end());
				for (const std::string& Identity : Dependencies)
				{
					const auto Found = Indices.find(Identity);
					if (Found == Indices.end())
					{
						OutError = std::format(
							"Scene output '{}' depends on missing output '{}'.",
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

		auto Abandon(std::vector<FPreparedSceneOutput>& Outputs) -> void
		{
			std::vector<DObject*> Objects;
			for (const auto& Output : Outputs) if (Output.Candidate) Objects.push_back(Output.Candidate);
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(Objects);
			FAssetCompilingManager::Get().FinishCompilationForObjects(Objects);
			for (FPreparedSceneOutput& Output : Outputs)
			{
				if (Output.Package && IsValid(Output.Package)) MarkObjectHierarchyAsGarbage(Output.Package);
				Output.Candidate = nullptr;
				Output.Package = nullptr;
			}
		}

		template<typename T>
		auto ConstructSceneCandidate(
			const FTopLevelAssetPath& AssetPath,
			T*& OutAsset,
			std::string& OutError) -> bool
		{
			OutAsset = nullptr;
			DPackage* Package = NewObject<DPackage>(nullptr, FName(AssetPath.GetAssetName()), EObjectFlags::Standalone);
			if (!Package || !Package->InitializePreparedAssetPackage(AssetPath.GetPackagePath()))
			{
				OutError = "The scene candidate package could not be created.";
				return false;
			}
			FStaticConstructObjectParameters Parameters{
				T::StaticClass(), Package, FName(AssetPath.GetAssetName()),
				sizeof(T), EObjectFlags::Public};
			DObject* Object = StaticConstructObject(Parameters);
			DObjectForceRegistration(Object);
			OutAsset = Cast<T>(Object);
			if (!OutAsset
				|| Package->FindTopLevelAsset(OutAsset->GetFName()) != OutAsset)
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
		// dependency-bound, validated, and ready for one atomic bundle save.
		auto CreateCandidate(FPreparedSceneOutput& Output, std::string& OutError) -> bool
		{
			bool bCreated = false;
			FTopLevelAssetPath AssetPath;
			if (!FTopLevelAssetPath::TryCreate(
				Output.AssetPath, Output.AssetPath.GetPackageName(), AssetPath))
			{
				OutError = "The scene output top-level asset path is invalid.";
				return false;
			}
			const ESceneOutputKind Kind = Output.Descriptor->Kind;
			if (Kind == ESceneOutputKind::StaticMesh)
			{
				DStaticMesh* Value = nullptr;
				bCreated = ConstructSceneCandidate(AssetPath, Value, OutError);
				Output.Candidate = Value;
			}
			else if (Kind == ESceneOutputKind::MaterialInstance)
			{
				DMaterialInstance* Value = nullptr;
				bCreated = ConstructSceneCandidate(AssetPath, Value, OutError);
				Output.Candidate = Value;
			}
			else if (Kind == ESceneOutputKind::Texture2D)
			{
				DTexture2D* Value = nullptr;
				bCreated = ConstructSceneCandidate(AssetPath, Value, OutError);
				Output.Candidate = Value;
			}
			if (!bCreated || !Output.Candidate)
			{
				if (OutError.empty()) OutError = "Scene candidate could not be created.";
				return false;
			}
			Output.Package = Output.Candidate->GetPackage();
			return Output.Package != nullptr;
		}

		auto ValidateCandidate(const FPreparedSceneOutput& Output, std::string& OutError) -> bool
		{
			if (const auto* Mesh = Cast<DStaticMesh>(Output.Candidate))
				return Mesh->GetRenderData() != nullptr;
			if (const auto* Texture = Cast<DTexture2D>(Output.Candidate))
				return Texture->GetPlatformData() != nullptr
					&& Texture->HasPlatformData();
			return Cast<DMaterialInstance>(Output.Candidate) != nullptr;
		}
	}

	auto ImportSceneAssets(
		std::string_view SourceFile,
		const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		FSceneImportResult& OutResult,
		const std::function<bool()>& IsCancellationRequested,
		const FSceneImportPublicationOptions& PublicationOptions) -> bool
	{
		OutResult = {};
		::Durin::AssetForge::Builtins::Private::FScopedSceneImportCancellation CancellationScope(
			IsCancellationRequested);
		const auto SettingsValidation = Settings.Validate();
		if (SourceFile.empty() || !DestinationDirectory.IsValid() || !SettingsValidation)
			return AddError(OutResult, EImportDiagnosticCategory::InvalidRequest,
				"scene-request", SourceFile.empty() || !DestinationDirectory.IsValid()
					? "Scene import request is invalid." : FormatStaticMeshImportSettingsError(SettingsValidation.Error));
		if (IsCanceled(IsCancellationRequested))
			return AddError(OutResult, EImportDiagnosticCategory::Canceled,
				"scene-capture", "Scene import was canceled before source capture.");

		const std::string RootFilename = std::filesystem::absolute(
			SourceFile).lexically_normal().generic_string();
		FSourceSnapshotBuilder SnapshotBuilder(IsCancellationRequested);
		if (!SnapshotBuilder.CaptureRootFilename(RootFilename, OutResult.Diagnostics)
			|| !SnapshotBuilder.DiscoverSourceDependencies(
				[](std::span<const FSourceSnapshotEntry> Sources,
					FDependencyRequestSink& Sink,
					std::vector<FImportDiagnostic>& Diagnostics) {
					return DiscoverSceneImportDependencies(Sources, Sink, Diagnostics);
				}, OutResult.Diagnostics))
			return AddError(OutResult, EImportDiagnosticCategory::InvalidSource,
				"scene-capture", "Scene source closure could not be captured.");
		auto Snapshot = SnapshotBuilder.Freeze(OutResult.Diagnostics);
		if (!Snapshot)
			return AddError(OutResult, EImportDiagnosticCategory::InvalidSource,
				"scene-capture", "Scene source closure could not be finalized.");
		if (IsCanceled(IsCancellationRequested))
			return AddError(OutResult, EImportDiagnosticCategory::Canceled,
				"scene-translation", "Scene import was canceled before translation.");

		FSceneImportPlan Data;
		if (!BuildScenePlan(*Snapshot, DestinationDirectory, Settings,
			Data, OutResult.Outputs, OutResult.Diagnostics, OutResult.Message))
		{
			if (!OutResult.Diagnostics.empty()
				&& !OutResult.Diagnostics.back().Message.empty())
				OutResult.Message = OutResult.Diagnostics.back().Message;
			return false;
		}
		std::vector<size_t> OutputOrder;
		if (!MakeStableOutputOrder(Data, OutputOrder, OutResult.Message))
			return AddError(OutResult, EImportDiagnosticCategory::DependencyCycle,
				"scene-order", OutResult.Message);
		std::vector<FPreparedSceneOutput> Prepared;
		Prepared.reserve(Data.Outputs.size());
		const FSourceSnapshotEntry* Root = Snapshot->FindSource("root");
		if (!Root)
			return AddError(OutResult, EImportDiagnosticCategory::InvalidSource,
				"scene-build", "Scene root source is unavailable.");
		for (const size_t Index : OutputOrder)
		{
			if (IsCanceled(IsCancellationRequested))
				return AddError(OutResult, EImportDiagnosticCategory::Canceled,
					"scene-build", "Scene import was canceled during product construction.");
			const FSceneOutputData& Descriptor = Data.Outputs[Index];
			const auto Summary = std::ranges::find(
				OutResult.Outputs, Descriptor.StableIdentity,
				&FImportOutputSummary::StableIdentity);
			if (Summary == OutResult.Outputs.end())
				return AddError(OutResult, EImportDiagnosticCategory::InvalidPlan,
					"scene-build", "Scene output mapping is incomplete.", Descriptor.StableIdentity);
			FPreparedSceneOutput& Output = Prepared.emplace_back();
			Output.Descriptor = &Descriptor;
			Output.AssetPath = Summary->AssetPath;
			std::string Error;
			if (Descriptor.Kind == ESceneOutputKind::Texture2D)
			{
				if (!BuildSceneImportTextureProduct(*Snapshot, Data, Descriptor,
					IsCancellationRequested, Output.Texture, Error))
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-build", std::move(Error), Descriptor.StableIdentity);
			}
			else if (Descriptor.Kind == ESceneOutputKind::StaticMesh)
			{
				if (const auto Initialized = Output.StaticMeshSource.Initialize(MakeStaticMeshDecodedGeometry(Data.Scene)); !Initialized)
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-build", FormatStaticMeshSourceError(Initialized.Error), Descriptor.StableIdentity);
				const auto Outcome = BuildStaticMeshAuthoredCandidate({
					.Source = Output.StaticMeshSource}, Output.StaticMesh,
					{.ShouldCancel = IsCancellationRequested});
				if (!Outcome)
					return AddError(OutResult, Outcome.GetStatus() == EStaticMeshBuildStatus::Cancelled
						? EImportDiagnosticCategory::Canceled : EImportDiagnosticCategory::CandidateFailure,
						"scene-build", FormatStaticMeshAuthoredBuildError(Outcome.Error), Descriptor.StableIdentity);
			}
		}

		if (IsCanceled(IsCancellationRequested))
			return AddError(OutResult, EImportDiagnosticCategory::Canceled,
				"scene-publication", "Scene import was canceled before publication.");
		std::lock_guard PublicationLock(GetScenePublicationMutex());
		// Match receipts in the destination before considering generated filenames.
		// An unrelated asset at a requested path is never replacement authority.
		std::unordered_map<std::string, DObject*> ExistingOutputs;
		std::vector<FPackagePath> ExistingPaths;
		const std::string Prefix = DestinationDirectory.ToString() + "/";
		for (const auto& [Path, Entry] : CaptureAssetCatalogSnapshot().Assets)
			if (Path.GetView().starts_with(Prefix)) ExistingPaths.push_back(Path);
		for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
			if (auto* Package = Cast<DPackage>(Object); Package &&
				Package->GetPackagePath().starts_with(Prefix) &&
				std::ranges::find(ExistingPaths, Package->GetPackagePathIdentity()) == ExistingPaths.end())
				ExistingPaths.push_back(Package->GetPackagePathIdentity());
		for (const auto& Path : ExistingPaths)
		{
			DObject* Object = nullptr;
			FObjectPath ObjectPath;
			if (!FObjectPath::TryCreate(Path.ToString() + "." + std::string(Path.GetPackageName()), ObjectPath) ||
				!LoadObject(ObjectPath, Object)) continue;
			const auto Identity = GetSceneOutputIdentity(Object, RootFilename);
			if (!Identity.empty() && !ExistingOutputs.emplace(Identity, Object).second)
				return AddError(OutResult, EImportDiagnosticCategory::Collision,
					"scene-publication", "Multiple saved outputs claim the same scene identity.", Identity);
		}
		for (FPreparedSceneOutput& Output : Prepared)
		{
			const auto Existing = ExistingOutputs.find(Output.Descriptor->StableIdentity);
			if (Existing != ExistingOutputs.end())
			{
				Output.Previous = Existing->second;
				Output.AssetPath = Output.Previous->GetPackage()->GetPackagePathIdentity();
				const bool bTypeMatches = Output.Descriptor->Kind == ESceneOutputKind::MaterialInstance
					? Cast<DMaterialInstance>(Output.Previous) != nullptr
					: Output.Descriptor->Kind == ESceneOutputKind::StaticMesh
						? Cast<DStaticMesh>(Output.Previous) != nullptr : Cast<DTexture2D>(Output.Previous) != nullptr;
				if (!bTypeMatches || Output.Previous->GetPackage()->GetTopLevelAssets().size() != 1)
					return AddError(OutResult, EImportDiagnosticCategory::Collision,
						"scene-publication", "The previous scene output has an incompatible package shape.", Output.Descriptor->StableIdentity);
			}
			else if (FindAssetExact(Output.AssetPath) || FindResidentPackage(Output.AssetPath))
			{
				// A changed derivation gets a distinct output; keep the old asset for
				// existing references instead of overwriting its identity.
				const auto Occupant = std::ranges::find_if(ExistingOutputs, [&](const auto& Entry) {
					return Entry.second->GetPackage()->GetPackagePathIdentity() == Output.AssetPath;
				});
				if (Occupant == ExistingOutputs.end() || Output.Descriptor->Kind != ESceneOutputKind::Texture2D ||
					!FPackagePath::TryCreate(Output.AssetPath.ToString() + "_" +
						FXxHash128::HashBuffer(std::as_bytes(std::span(Output.Descriptor->StableIdentity))).ToString(), Output.AssetPath) ||
					FindAssetExact(Output.AssetPath) || FindResidentPackage(Output.AssetPath))
					return AddError(OutResult, EImportDiagnosticCategory::Collision,
						"scene-publication", "Scene output path is occupied by an unrelated output.", Output.Descriptor->StableIdentity);
			}
			std::ranges::find(OutResult.Outputs, Output.Descriptor->StableIdentity,
				&FImportOutputSummary::StableIdentity)->AssetPath = Output.AssetPath;
		}

		for (FPreparedSceneOutput& Output : Prepared)
		{
			std::string Error;
			if (!CreateCandidate(Output, Error))
			{
				Abandon(Prepared);
				return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
					"scene-materialization", std::move(Error), Output.Descriptor->StableIdentity);
			}
			const FSceneOutputData& Descriptor = *Output.Descriptor;
			if (Descriptor.Kind == ESceneOutputKind::Texture2D)
			{
				const FXxHash128 SourceHash = Output.Texture.EncodedSourceHash;
				const std::string SourcePhysicalPath = Output.Texture.SourceFilename;
				const FAssetPathResult PackageResolution =
					FMountPaths::ResolveAssetPath(
						Output.AssetPath.GetView(), EMountPathExistence::AllowMissing);
				if (!PackageResolution)
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", PackageResolution.Message,
						Descriptor.StableIdentity);
				}
				std::filesystem::path PackagePath = PackageResolution.PhysicalPath;
				PackagePath += ".dasset";
				std::string SourceHint;
				ESourceHintBase HintBase;
				if (const auto Hint = MakeSourceHint(
					SourcePhysicalPath, PackagePath.generic_string(), HintBase,
					SourceHint); !Hint)
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", FormatSourceHintError(Hint.Error), Descriptor.StableIdentity);
				}
				auto* Texture = Cast<DTexture2D>(Output.Candidate);
				FTexture2DBuildProduct& Product = Output.Texture.Product;
				const FTexture2DBuildSettings& Settings = Output.Texture.Settings;
				auto PlatformData = std::make_unique<FTexturePlatformData>(
					std::move(Product.PlatformData));
				if (!PlatformData->IsValid())
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", "Texture platform data is invalid.", Descriptor.StableIdentity);
				}
				if (const auto Validation = ValidateTexture2DBuildSettings(Settings); !Validation)
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", FormatTexture2DInputError(Validation.Error), Descriptor.StableIdentity);
				}
				Texture->SetSource(Output.Texture.SourceData);
				Texture->SetBuildSettings(Settings.Usage, ResolveTexture2DSRGB(Settings),
					Settings.MaxResolution, Settings.CompressionQuality,
					Settings.AlphaMipMode, Settings.AlphaCoverageThreshold);
				Texture->SetPlatformData(std::move(PlatformData));
				Texture->UpdateResource();
				Texture->MarkPackageDirty();
				FAssetImportDataState ImportState;
				ImportState.SourceData.Sources.push_back({
					.Role = "source",
					.DisplayLabel = std::filesystem::path(SourcePhysicalPath).filename().generic_string(),
					.Hint = SourceHint,
					.HintBase = HintBase,
					.ContentHashLow = SourceHash.HashLow,
					.ContentHashHigh = SourceHash.HashHigh,
					.ByteCount = Output.Texture.SourceFileSize});
				auto* ImportData = NewObject<DSceneImportData>(
					Output.Candidate, "AssetImportData");
				ImportState.SourceData.Normalize();
				const auto Validation = ImportState.Validate();
				if (!ImportData || !Validation)
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", !ImportData
							? "Scene texture import data could not be published." : FormatAssetImportDataError(Validation.Error),
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
				auto* Mesh = Cast<DStaticMesh>(Output.Candidate);
				auto* ImportData = NewObject<DSceneImportData>(Mesh, "AssetImportData");
				if (!ImportData)
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", "Scene mesh import data could not be created.", Descriptor.StableIdentity);
				}
				ImportData->SourceIdentity = RootFilename;
				ImportData->OutputIdentity = Descriptor.StableIdentity;
				if (const auto Applied = ApplyStaticMeshAuthoredCandidate(*Mesh, std::move(Output.StaticMesh),
					CaptureStaticMeshReconciliation(*Mesh), true, {}, ImportData); !Applied)
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", FormatStaticMeshApplicationError(Applied.Error), Descriptor.StableIdentity);
				}
			}
		}

		auto FindOutput = [&](std::string_view Identity) -> FPreparedSceneOutput* {
			const auto It = std::ranges::find_if(Prepared,
				[&](const FPreparedSceneOutput& Value) {
					return Value.Descriptor->StableIdentity == Identity;
				});
			return It == Prepared.end() ? nullptr : &*It;
		};
		auto FindMaterial = [&](uint32 SourceIndex) -> DMaterialInterface* {
			const auto It = std::ranges::find_if(Prepared,
				[&](const FPreparedSceneOutput& Value) {
					return Value.Descriptor->Kind == ESceneOutputKind::MaterialInstance
						&& Value.Descriptor->SourceIndex == SourceIndex;
				});
			return It == Prepared.end() ? nullptr : Cast<DMaterialInterface>(It->Candidate);
		};

		FGeneratedParentScope GeneratedParents;
		auto ResolveParent = [&](const FImportedSurfaceRecipe& Recipe, std::string& Error) -> DMaterial* {
			const auto Destination = DestinationDirectory.GetView();
			const auto MountEnd = Destination.find('/', 1);
			const auto Name = "Surface_v1_" + FXxHash128::HashBuffer(std::as_bytes(std::span(Recipe.CanonicalKey))).ToString();
			FPackagePath Path;
			if (const auto PathValidation = FPackagePath::TryCreate(std::string(Destination.substr(0, MountEnd)) +
				"/Materials/ImportedParents/" + Name, Path); !PathValidation) { Error = FormatObjectError(PathValidation.Error); return nullptr; }
			DMaterial* Parent = nullptr;
			const auto Local = std::ranges::find_if(GeneratedParents.Packages, [&](const DPackage* Package) {
				return Package->GetPackagePathIdentity() == Path;
			});
			if (Local != GeneratedParents.Packages.end()) Parent = Cast<DMaterial>((*Local)->FindTopLevelAsset(FName(Name)));
			else if (auto* Package = FindResidentPackage(Path))
				Parent = Cast<DMaterial>(Package->FindTopLevelAsset(FName(Name)));
			else if (FindAssetExact(Path))
			{
				FObjectPath ObjectPath;
				if (const auto PathValidation = FObjectPath::TryCreate(Path.ToString() + "." + Name, ObjectPath); !PathValidation)
				{
					Error = FormatObjectError(PathValidation.Error);
					return nullptr;
				}
				if (!LoadObject(ObjectPath, Parent)) return nullptr;
			}
			else
			{
				FTopLevelAssetPath AssetPath;
				if (!FTopLevelAssetPath::TryCreate(Path, Name, AssetPath) ||
					!ConstructSceneCandidate(AssetPath, Parent, Error)) return nullptr;
				GeneratedParents.Packages.push_back(Parent->GetPackage());
				Parent->SetEditCompileMode(EMaterialEditCompileMode::Manual);
				const auto Valid = Recipe.Graph.Apply(*Parent);
				if (!Valid)
				{
					Error = Valid.Diagnostics.empty() ? "Generated surface program is invalid." : Durin::FormatMaterialError(Valid.Diagnostics.front().Error);
					return nullptr;
				}
				if (!Parent->SetImportProvenance({.RecipeId = "Durin.ImportedSurface", .RecipeVersion = 1,
						.StructuralKey = Recipe.CanonicalKey})) return nullptr;
			}
			if (!Parent || Parent->GetImportProvenance().RecipeId != "Durin.ImportedSurface" ||
				Parent->GetImportProvenance().RecipeVersion != 1 ||
				Parent->GetImportProvenance().StructuralKey != Recipe.CanonicalKey ||
				!Recipe.Graph.MatchesGraph(*Parent) ||
				Parent->GetStaticProperties() != FMaterialStaticProperties{})
			{
				Error = "Generated surface parent path is occupied or its recipe was modified: " + Path.ToString();
				return nullptr;
			}
			return Parent;
		};
		for (FPreparedSceneOutput& Output : Prepared)
		{
			const FSceneOutputData& Descriptor = *Output.Descriptor;
			std::string Error;
			if (Descriptor.Kind == ESceneOutputKind::MaterialInstance)
			{
				const auto Imported = std::ranges::find(Data.Scene.Materials,
					Descriptor.SourceIndex, &FImportedMaterial::SourceMaterialIndex);
				const auto Roles = MakeSceneSurfaceRoles(Data, Descriptor);
				const auto Recipe = MakeImportedSurfaceRecipe(Roles);
				auto* Standard = ResolveParent(Recipe, Error);
				if (Imported == Data.Scene.Materials.end() || !Standard)
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
						"scene-dependency-binding", Error.empty()
							? "Scene material dependency is unavailable." : std::move(Error),
						Descriptor.StableIdentity);
				}
				auto* Material = Cast<DMaterialInstance>(Output.Candidate);
				FMaterialPropertyOverrides Overrides;
				Overrides.bOverrideBlendMode = true;
				Overrides.bOverrideOpacityMaskThreshold = true;
				Overrides.bOverrideTwoSided = true;
				auto& Properties = Overrides.Values;
				Properties.BlendMode = Imported->AlphaMode == EImportedAlphaMode::Mask
					? EMaterialBlendMode::Masked : Imported->AlphaMode == EImportedAlphaMode::Blend
						? EMaterialBlendMode::Translucent : EMaterialBlendMode::Opaque;
				Properties.bTwoSided = Imported->bDoubleSided;
				Properties.OpacityMaskThreshold = Imported->AlphaCutoff;
				if (!Material || !Material->SetParentAndPropertyOverrides(Standard, Overrides))
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
						"scene-dependency-binding", "Scene material parent could not be applied.",
						Descriptor.StableIdentity);
				}
				FMaterialImportProvenance Receipt{.RecipeId = "Durin.ImportedSurface", .RecipeVersion = 1,
					.StructuralKey = Recipe.CanonicalKey, .SourceIdentity = RootFilename, .OutputIdentity = Descriptor.StableIdentity};
				for (const auto& Owner : Recipe.Owners)
				{
					using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
					const auto* Definition = Standard->FindParameterDefinition(Owner.ParameterId);
					require(Definition);
					const auto& Role = Roles[static_cast<uint32>(Owner.Role)];
					FMaterialParameterValue Value;
					if (Owner.Kind == Kind::Texture)
					{
						auto* Texture = FindOutput(Role.Sample->ResourceIdentity);
						require(Texture && Cast<DTexture2D>(Texture->Candidate));
						Value = FMaterialParameterValue::MakeTexture(Cast<DTexture2D>(Texture->Candidate),
							Role.Sample->Sampler, Definition->Value.GetTexture().TextureFallback);
					}
					else
					{
						const auto Literal = Owner.Kind == Kind::Value ? Role.Value :
							Owner.Kind == Kind::UVChannel ? Role.Sample->UVChannel :
							Owner.Kind == Kind::UVScale ? Role.Sample->UVScale :
							Owner.Kind == Kind::UVOffset ? Role.Sample->UVOffset : Role.Sample->UVRotation;
						Value = Definition->Type == EMaterialParameterType::Vector4 ? FMaterialParameterValue::MakeVector4({Literal.X, Literal.Y, Literal.Z, Literal.W}) :
							FMaterialParameterValue::MakeScalar(Literal.X);
					}
					if (const auto Applied = Material->SetParameterValue(Owner.ParameterId, Value); !Applied)
					{
						Abandon(Prepared);
						return AddError(OutResult, EImportDiagnosticCategory::ValidationFailure,
							"scene-material-parameters", FormatMaterialError(Applied.Error), Descriptor.StableIdentity);
					}
				}
				if (!Material->SetImportProvenance(std::move(Receipt)))
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::ValidationFailure,
						"scene-material-parameters", "Scene material import receipt is invalid.", Descriptor.StableIdentity);
				}
			}
			else if (Descriptor.Kind == ESceneOutputKind::StaticMesh)
			{
				auto* Mesh = Cast<DStaticMesh>(Output.Candidate);
				for (const FSceneOutputData& Candidate : Data.Outputs)
					if (Candidate.Kind == ESceneOutputKind::MaterialInstance)
					{
						FPreparedSceneOutput* Material = FindOutput(Candidate.StableIdentity);
						uint32 SlotIndex = 0;
						if (!Material || !ResolveImportedMaterialSlot(*Mesh, Candidate.SourceIndex,
							SlotIndex, Error))
						{
							Abandon(Prepared);
							return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
								"scene-dependency-binding", Error.empty()
									? "Scene material dependency is unavailable." : std::move(Error),
								Descriptor.StableIdentity);
						}
						Mesh->SetMaterialSlotDefaultMaterial(SlotIndex,
							Cast<DMaterialInstance>(Material->Candidate));
					}
			}
			Output.Candidate->MarkPackageDirty();
			if (!ValidateCandidate(Output, Error))
			{
				Abandon(Prepared);
				return AddError(OutResult, EImportDiagnosticCategory::ValidationFailure,
					"scene-validation", Error.empty()
						? "Scene candidate has no validated runtime data." : std::move(Error),
					Descriptor.StableIdentity);
			}
		}

		std::vector<DObject*> Materials;
		std::vector<DMaterial*> SelectedParents;
		for (const auto& Output : Prepared)
			if (auto* Instance = Cast<DMaterialInstance>(Output.Candidate))
			{
				auto* Parent = Cast<DMaterial>(Instance->GetParent());
				if (std::ranges::find(SelectedParents, Parent) == SelectedParents.end()) SelectedParents.push_back(Parent);
			}
		for (auto* Parent : SelectedParents)
		{
			require(Parent);
			if (!Parent->CompileEdits())
			{
				Abandon(Prepared);
				return AddError(OutResult, EImportDiagnosticCategory::ValidationFailure,
					"scene-material-compile", "Generated surface parent could not be compiled.");
			}
			Materials.push_back(Parent);
		}
		for (auto& Output : Prepared)
			if (Cast<DMaterialInstance>(Output.Candidate)) Materials.push_back(Output.Candidate);
		for (auto* Material : Materials)
			if (auto* Instance = Cast<DMaterialInstance>(Material); Instance &&
				(Instance->GetMaterialCompileStatus().HasUnsubmittedEdits() ||
				 Instance->GetMaterialCompileStatus().State == EMaterialCompileState::NeverRequested) && !RequestMaterialRecompile(*Instance))
			{
				Abandon(Prepared);
				return AddError(OutResult, EImportDiagnosticCategory::ValidationFailure,
					"scene-material-compile", "Private material candidate could not be compiled.");
			}
		FAssetCompilingManager::Get().FinishCompilationForObjects(Materials);
		for (auto* Package : GeneratedParents.Packages)
			Cast<DMaterial>(Package->FindTopLevelAsset(FName(Package->GetPackagePathIdentity().GetPackageName())))
				->SetEditCompileMode(EMaterialEditCompileMode::Immediate);
		for (auto* Object : Materials)
		{
			auto* Material = Cast<DMaterialInterface>(Object);
			if (Material->GetMaterialCompileStatus().IsCurrent() && Material->GetAcceptedCompiledProgram()) continue;
			const auto Diagnostics = Material->GetMaterialCompileDiagnostics();
			const std::string Message = Diagnostics.empty() ? std::format("Scene material variant is not ready: {} (state {}).",
				Material->GetObjectPath(), static_cast<uint32>(Material->GetMaterialCompileStatus().State))
				: Durin::FormatMaterialError(Diagnostics.front().Source.Error);
			Abandon(Prepared);
			return AddError(OutResult, EImportDiagnosticCategory::ValidationFailure,
				"scene-material-compile", Message);
		}

		std::vector<DPackage*> Packages = GeneratedParents.Packages;
		Packages.reserve(Prepared.size());
		for (const FPreparedSceneOutput& Output : Prepared) Packages.push_back(Output.Package);
		if (IsCanceled(IsCancellationRequested))
		{
			Abandon(Prepared);
			return AddError(OutResult, EImportDiagnosticCategory::Canceled,
				"scene-publication", "Scene import was canceled before persistence.");
		}
		FAssetBundleSaveOptions SaveOptions{.ShouldFail = PublicationOptions.ShouldFail,
			.bRollbackOnRegistryFailure = true};
		if (!Packages.empty()) SaveOptions.RootPackage = Packages.back();
		FObjectGraphReplacement Publication;
		std::vector<FObjectReplacementPackagePair> Pairs;
		for (auto* Package : GeneratedParents.Packages) Pairs.push_back({nullptr, Package});
		for (const auto& Output : Prepared)
			Pairs.push_back({Output.Previous ? Output.Previous->GetPackage() : nullptr, Output.Package});
		std::vector<DObject*> ExternalConsumers;
		std::vector<DObject*> DependentMaterials;
		for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
			if (std::ranges::none_of(Pairs, [&](const auto& Pair) { return Pair.Current && Object->GetPackage() == Pair.Current; }))
			{
				ExternalConsumers.push_back(Object);
				if (auto* Material = Cast<DMaterialInstance>(Object))
					for (auto* Parent = Material->GetParent(); Parent; Parent = Parent->GetParent())
						if (std::ranges::any_of(Prepared, [&](const auto& Output) { return Output.Previous == Parent; }))
						{
							DependentMaterials.push_back(Material);
							break;
						}
			}
		auto PreviousCompilations = DependentMaterials;
		for (const auto& Output : Prepared) if (Output.Previous) PreviousCompilations.push_back(Output.Previous);
		FAssetCompilingManager::Get().FinishCompilationForObjects(PreviousCompilations);
		FAssetResult PersistenceResult;
		auto Published = Publication.Prepare(Pairs, {}, {.MaximumPackages = 4096});
		if (Published)
		{
			SaveOptions.PreparedPublication = &Publication;
			Published = Publication.TryCommit([&]() -> FObjectReplacementResult {
				PersistenceResult = SavePackagesAtomically(Packages, SaveOptions);
				if (!PersistenceResult) return {{.Code = EObjectReplacementError::ParticipantRejected,
					.Reason = EObjectReplacementReason::PersistenceRejected}};
				return {};
			});
		}
		if (!Published)
		{
			Publication.Abort();
			Abandon(Prepared);
			return AddError(OutResult, EImportDiagnosticCategory::PersistenceFailure,
				"scene-persistence", !PersistenceResult ? PersistenceResult.Message : FormatObjectReplacementError(Published.Error));
		}
		for (auto* Package : Packages) Package->MarkAsPublished();
		if (std::ranges::any_of(Prepared, [](const auto& Output) { return Output.Previous != nullptr; }))
		{
			// External instance variants must follow their newly bound parent graph.
			for (auto* Object : DependentMaterials) RequestMaterialRecompile(*Cast<DMaterialInterface>(Object));
			FAssetCompilingManager::Get().FinishCompilationForObjects(DependentMaterials);
			for (auto* Object : ExternalConsumers)
			{
				if (auto* Material = Cast<DMaterialInterface>(Object)) Material->RefreshReloadedAssetBindings();
				if (auto* Cloud = Cast<DVolumetricCloudComponent>(Object)) Cloud->RefreshReloadedAssetBindings();
				if (auto* Sky = Cast<DSkyLightComponent>(Object)) Sky->RefreshReloadedAssetBindings();
				if (auto* Mesh = Cast<DStaticMeshComponent>(Object)) Mesh->RefreshReloadedAssetBindings();
				else if (auto* Primitive = Cast<DPrimitiveComponent>(Object))
					Primitive->MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
			}
			FlushRenderingCommands();
		}
		require(Publication.Retire());
		GeneratedParents.bRetain = true;
		OutResult.bSucceeded = true;
		OutResult.bPersisted = true;
		OutResult.Message.clear();
		return true;
	}
}
