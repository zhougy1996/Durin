#include "AssetForge/Builtins/SceneImport.h"

#include "Animation/AnimationClip.h"
#include "Asset.h"
#include "Asset/AssetOperations.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/SourceHint.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Package.h"
#include "DObject/DObjectGlobals.h"
#include "AssetForge/Builtins/Texture2DImportData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "SceneImportInternal.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "StaticMeshImportAdapter.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		struct FPreparedSceneOutput
		{
			const FSceneOutputData* Descriptor = nullptr;
			FAssetPath AssetPath;
			FStaticMeshBuildProduct StaticMesh;
			FSceneTextureBuildProduct Texture;
			Asset::FSkeletalMeshBuildProduct SkeletalMesh;
			Asset::FAnimationClipBuildProduct Animation;
			DObject* Candidate = nullptr;
			DPackage* Package = nullptr;
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
			const FSceneProviderPlanData& Data,
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
				if (Output.Kind == ESceneOutputKind::SkeletalMesh
					|| Output.Kind == ESceneOutputKind::AnimationClip)
					Dependencies.push_back(Output.SkeletonIdentity);
				if (Output.Kind == ESceneOutputKind::StaticMesh
					|| Output.Kind == ESceneOutputKind::SkeletalMesh)
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
			std::vector<FAssetPath> Paths;
			for (FPreparedSceneOutput& Output : Outputs)
			{
				if (Output.Package) Paths.push_back(Output.AssetPath);
				Output.Candidate = nullptr;
				Output.Package = nullptr;
			}
			for (auto It = Paths.rbegin(); It != Paths.rend(); ++It)
				(void)Asset::UnloadPackage(*It, Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
		}

		auto CreateCandidate(FPreparedSceneOutput& Output, std::string& OutError) -> bool
		{
			Asset::FAssetResult Created;
			const ESceneOutputKind Kind = Output.Descriptor->Kind;
			if (Kind == ESceneOutputKind::StaticMesh)
			{
				DStaticMesh* Value = nullptr;
				Created = Asset::CreateAsset(Output.AssetPath, Value);
				Output.Candidate = Value;
			}
			else if (Kind == ESceneOutputKind::MaterialInstance)
			{
				DMaterialInstance* Value = nullptr;
				Created = Asset::CreateAsset(Output.AssetPath, Value);
				Output.Candidate = Value;
			}
			else if (Kind == ESceneOutputKind::Skeleton)
			{
				DSkeleton* Value = nullptr;
				Created = Asset::CreateAsset(Output.AssetPath, Value);
				Output.Candidate = Value;
			}
			else if (Kind == ESceneOutputKind::SkeletalMesh)
			{
				DSkeletalMesh* Value = nullptr;
				Created = Asset::CreateAsset(Output.AssetPath, Value);
				Output.Candidate = Value;
			}
			else if (Kind == ESceneOutputKind::AnimationClip)
			{
				DAnimationClip* Value = nullptr;
				Created = Asset::CreateAsset(Output.AssetPath, Value);
				Output.Candidate = Value;
			}
			else if (Kind == ESceneOutputKind::Texture2D)
			{
				DTexture2D* Value = nullptr;
				Created = Asset::CreateAsset(Output.AssetPath, Value);
				Output.Candidate = Value;
			}
			if (!Created || !Output.Candidate)
			{
				OutError = Created.Message.empty()
					? "Scene candidate could not be created." : Created.Message;
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
					&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready;
			if (const auto* Skeleton = Cast<DSkeleton>(Output.Candidate))
				return Skeleton->Validate(OutError);
			if (const auto* Mesh = Cast<DSkeletalMesh>(Output.Candidate))
				return Mesh->Validate(OutError);
			if (const auto* Clip = Cast<DAnimationClip>(Output.Candidate))
				return Clip->Validate(OutError);
			return Cast<DMaterialInstance>(Output.Candidate) != nullptr;
		}
	}

	auto ImportSceneAssets(
		std::string_view SourceFile,
		const FAssetPath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		FSceneImportResult& OutResult,
		const std::function<bool()>& IsCancellationRequested) -> bool
	{
		OutResult = {};
		::Durin::AssetForge::Builtins::Private::FScopedSceneImportCancellation CancellationScope(
			IsCancellationRequested);
		if (SourceFile.empty() || !DestinationDirectory.IsValid()
			|| !Settings.IsValid(&OutResult.Message))
			return AddError(OutResult, EImportDiagnosticCategory::InvalidRequest,
				"scene-request", OutResult.Message.empty()
					? "Scene import request is invalid." : OutResult.Message);
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

		std::shared_ptr<const FSceneProviderPlanData> Data;
		if (!BuildSceneImportPlanData(*Snapshot, DestinationDirectory, Settings,
			Data, OutResult.Outputs, OutResult.Diagnostics, OutResult.Message))
		{
			if (!OutResult.Diagnostics.empty()
				&& !OutResult.Diagnostics.back().Message.empty())
				OutResult.Message = OutResult.Diagnostics.back().Message;
			return false;
		}
		std::vector<size_t> OutputOrder;
		if (!MakeStableOutputOrder(*Data, OutputOrder, OutResult.Message))
			return AddError(OutResult, EImportDiagnosticCategory::DependencyCycle,
				"scene-order", OutResult.Message);
		std::vector<FPreparedSceneOutput> Prepared;
		Prepared.reserve(Data->Outputs.size());
		const FSourceSnapshotEntry* Root = Snapshot->FindSource("root");
		if (!Root)
			return AddError(OutResult, EImportDiagnosticCategory::InvalidSource,
				"scene-build", "Scene root source is unavailable.");
		for (const size_t Index : OutputOrder)
		{
			if (IsCanceled(IsCancellationRequested))
				return AddError(OutResult, EImportDiagnosticCategory::Canceled,
					"scene-build", "Scene import was canceled during product construction.");
			const FSceneOutputData& Descriptor = Data->Outputs[Index];
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
				if (!BuildSceneImportTextureProduct(*Snapshot, *Data, Descriptor,
					IsCancellationRequested, Output.Texture, Error))
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-build", std::move(Error), Descriptor.StableIdentity);
			}
			else if (Descriptor.Kind == ESceneOutputKind::StaticMesh)
			{
				FStaticMeshSourceImportData Provenance{
					.SourceFilename = Root->Filename,
					.SourceContentHash = Root->ContentHash.ToString(),
					.ImporterId = std::string(SceneImporterId),
					.ImporterVersion = 1,
					.ImportSettings = Data->MeshSettings};
				if (!Asset::FStaticMeshBuildOperations::BuildImportedProduct(
					{.StableObjectPath = Output.AssetPath.ToString()},
					MakeStaticMeshImportedData(Data->Scene), std::move(Provenance),
					Root->Filename, Output.StaticMesh, Error))
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-build", std::move(Error), Descriptor.StableIdentity);
			}
			else if (Descriptor.Kind == ESceneOutputKind::SkeletalMesh)
			{
				if (Descriptor.SourceIndex >= Data->Scene.SkeletalMeshes.size())
					return AddError(OutResult, EImportDiagnosticCategory::InvalidPlan,
						"scene-build", "Scene SkeletalMesh mapping is invalid.", Descriptor.StableIdentity);
				const FImportedSkeletalMeshData& Imported =
					Data->Scene.SkeletalMeshes[Descriptor.SourceIndex];
				if (Imported.SkeletonIndex >= Data->Scene.Skeletons.size())
					return AddError(OutResult, EImportDiagnosticCategory::InvalidPlan,
						"scene-build", "Scene Skeleton mapping is invalid.", Descriptor.StableIdentity);
				const FImportedSkeletonData& Skeleton = Data->Scene.Skeletons[Imported.SkeletonIndex];
				FSkeletalMeshImportedData Canonical;
				if (!Canonical.Capture(*Imported.Payload,
					static_cast<uint32>(Skeleton.Bones.size()),
					static_cast<uint32>(Imported.MaterialSlots.size()), Error))
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-build", std::move(Error), Descriptor.StableIdentity);
				Asset::FSkeletalMeshBuildKeyInput Key;
				static_cast<Asset::FSkeletalBuildKeyFields&>(Key) = {
					.ProviderIdentity = "CanonicalSkeletalMesh",
					.ProviderVersion = SkeletalMeshImportedDataSchemaVersion,
					.ImportedDataIdentity = Canonical.GetIdentity(),
					.StableOutputIdentity = Output.AssetPath.ToString(),
					.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
					.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
					.TargetProfile = ESkeletalPayloadTargetProfile::Game};
				if (!Asset::BuildSkeletalMeshProduct({
					.SkeletonBoneCount = static_cast<uint32>(Skeleton.Bones.size()),
					.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
					.MeshNodeBindTransform = Imported.MeshNodeBindTransform,
					.MaterialSlotCount = static_cast<uint32>(Imported.MaterialSlots.size()),
					.Payload = Imported.Payload, .KeyInput = std::move(Key)},
					Output.SkeletalMesh, Error))
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-build", std::move(Error), Descriptor.StableIdentity);
			}
			else if (Descriptor.Kind == ESceneOutputKind::AnimationClip)
			{
				if (Descriptor.SourceIndex >= Data->Scene.AnimationClips.size())
					return AddError(OutResult, EImportDiagnosticCategory::InvalidPlan,
						"scene-build", "Scene AnimationClip mapping is invalid.", Descriptor.StableIdentity);
				const FImportedAnimationClipData& Imported =
					Data->Scene.AnimationClips[Descriptor.SourceIndex];
				if (Imported.SkeletonIndex >= Data->Scene.Skeletons.size())
					return AddError(OutResult, EImportDiagnosticCategory::InvalidPlan,
						"scene-build", "Scene Skeleton mapping is invalid.", Descriptor.StableIdentity);
				const FImportedSkeletonData& Skeleton = Data->Scene.Skeletons[Imported.SkeletonIndex];
				FAnimationClipImportedData Canonical;
				if (!Canonical.Capture(*Imported.Payload,
					static_cast<uint32>(Skeleton.Bones.size()), Error))
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-build", std::move(Error), Descriptor.StableIdentity);
				Asset::FAnimationClipBuildKeyInput Key;
				static_cast<Asset::FSkeletalBuildKeyFields&>(Key) = {
					.ProviderIdentity = "CanonicalAnimationClip",
					.ProviderVersion = AnimationClipImportedDataSchemaVersion,
					.ImportedDataIdentity = Canonical.GetIdentity(),
					.StableOutputIdentity = Output.AssetPath.ToString(),
					.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
					.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
					.TargetProfile = ESkeletalPayloadTargetProfile::Game};
				if (!Asset::BuildAnimationClipProduct({
					.SkeletonBoneCount = static_cast<uint32>(Skeleton.Bones.size()),
					.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
					.ClipName = FName(Imported.SuggestedName), .Payload = Imported.Payload,
					.KeyInput = std::move(Key)}, Output.Animation, Error))
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-build", std::move(Error), Descriptor.StableIdentity);
			}
		}

		if (IsCanceled(IsCancellationRequested))
			return AddError(OutResult, EImportDiagnosticCategory::Canceled,
				"scene-publication", "Scene import was canceled before publication.");
		std::lock_guard PublicationLock(GetImportPublicationMutex());
		for (const FPreparedSceneOutput& Output : Prepared)
			if (Asset::FindAssetExact(Output.AssetPath)
				|| Asset::FindResidentPackage(Output.AssetPath))
				return AddError(OutResult, EImportDiagnosticCategory::Collision,
					"scene-publication", std::format(
						"Scene output '{}' already exists.", Output.AssetPath.ToString()),
					Output.Descriptor->StableIdentity);

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
			if (Descriptor.Kind == ESceneOutputKind::Skeleton)
			{
				if (Descriptor.SourceIndex >= Data->Scene.Skeletons.size()
					|| !Cast<DSkeleton>(Output.Candidate)->InitializeCanonicalBones(
						Data->Scene.Skeletons[Descriptor.SourceIndex].Bones, Error))
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", std::move(Error), Descriptor.StableIdentity);
				}
			}
			else if (Descriptor.Kind == ESceneOutputKind::Texture2D)
			{
				const FXxHash128 SourceHash{
					.HashLow = Output.Texture.Product.SourceContentHashLow,
					.HashHigh = Output.Texture.Product.SourceContentHashHigh};
				const std::string SourcePhysicalPath = Output.Texture.SourceFilename;
				const PathUtilities::FAssetPathResult PackageResolution =
					PathUtilities::ResolveAssetPath(
						Output.AssetPath.GetView(), PathUtilities::EPathExistence::AllowMissing);
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
				if (!MakeSourceHint(
					SourcePhysicalPath, PackagePath.generic_string(), HintBase,
					SourceHint, Error))
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", std::move(Error), Descriptor.StableIdentity);
				}
				if (!Asset::PublishTexture2DProduct(*Cast<DTexture2D>(Output.Candidate),
					std::move(Output.Texture.Product), {}, Error))
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", std::move(Error), Descriptor.StableIdentity);
				}
				FTexture2DImportDataState ImportState;
				ImportState.SourceData.Sources.push_back({
					.Role = "source",
					.DisplayLabel = std::filesystem::path(SourcePhysicalPath).filename().generic_string(),
					.Hint = SourceHint,
					.HintBase = HintBase,
					.ContentHashLow = SourceHash.HashLow,
					.ContentHashHigh = SourceHash.HashHigh,
					.ByteCount = Output.Texture.SourceFileSize});
				ImportState.DecoderId = "DurinImage";
				ImportState.DecoderVersion = 1;
				auto* ImportData = NewObject<DTexture2DImportData>(
					Output.Candidate, "Texture2DImportData");
				if (!ImportData || !ImportData->SetState(std::move(ImportState), Error)
					|| !Cast<DTexture2D>(Output.Candidate)->PublishAssetImportData(
						*ImportData, Error))
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
						"scene-materialization", Error.empty()
							? "Scene texture import data could not be published." : std::move(Error),
						Descriptor.StableIdentity);
				}
			}
			else if (Descriptor.Kind == ESceneOutputKind::StaticMesh
				&& !Asset::FStaticMeshBuildOperations::PublishImportedProduct(
					*Cast<DStaticMesh>(Output.Candidate), std::move(Output.StaticMesh), Error))
			{
				Abandon(Prepared);
				return AddError(OutResult, EImportDiagnosticCategory::CandidateFailure,
					"scene-materialization", std::move(Error), Descriptor.StableIdentity);
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

		for (FPreparedSceneOutput& Output : Prepared)
		{
			const FSceneOutputData& Descriptor = *Output.Descriptor;
			std::string Error;
			if (Descriptor.Kind == ESceneOutputKind::MaterialInstance)
			{
				DMaterial* Standard = nullptr;
				FAssetPath StandardPath;
				const auto Imported = std::ranges::find(Data->Scene.Materials,
					Descriptor.SourceIndex, &FImportedMaterial::SourceMaterialIndex);
				if (Imported == Data->Scene.Materials.end()
					|| !FAssetPath::TryCreate(ImportedSurfaceMaterialPath, StandardPath, &Error)
					|| !Asset::LoadAsset(StandardPath, Standard) || !Standard)
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
						"scene-dependency-binding", Error.empty()
							? "Scene material dependency is unavailable." : std::move(Error),
						Descriptor.StableIdentity);
				}
				(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Standard);
				auto* Material = Cast<DMaterialInstance>(Output.Candidate);
				FMaterialStaticProperties Properties = Standard->GetStaticProperties();
				Properties.BlendMode = Imported->AlphaMode == EImportedAlphaMode::Mask
					? EMaterialBlendMode::Masked : Imported->AlphaMode == EImportedAlphaMode::Blend
						? EMaterialBlendMode::Translucent : EMaterialBlendMode::Opaque;
				Properties.bTwoSided = Imported->bDoubleSided;
				Properties.OpacityMaskThreshold = Imported->AlphaCutoff;
				if (!Material || !Material->SetParent(Standard)
					|| !Material->SetStaticPropertiesOverride(Properties))
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
						"scene-dependency-binding", "Scene material parent could not be applied.",
						Descriptor.StableIdentity);
				}
				(void)Material->SetVectorParameterValue(
					MaterialParameters::BaseColorName(), FVector3(Imported->BaseColorFactor));
				(void)Material->SetScalarParameterValue(
					MaterialParameters::OpacityName(), Imported->BaseColorFactor.a);
				(void)Material->SetScalarParameterValue(
					MaterialParameters::MetallicName(), Imported->MetallicFactor);
				(void)Material->SetScalarParameterValue(
					MaterialParameters::RoughnessName(), Imported->RoughnessFactor);
				const std::array<const FName*, 8> Names{
					&MaterialParameters::BaseColorTextureName(),
					&MaterialParameters::NormalTextureName(),
					&MaterialParameters::MetallicTextureName(),
					&MaterialParameters::RoughnessTextureName(),
					&MaterialParameters::AmbientOcclusionTextureName(),
					&MaterialParameters::EmissiveTextureName(),
					&MaterialParameters::OpacityTextureName(),
					&MaterialParameters::OpacityMaskTextureName()};
				for (const FSceneMaterialTextureBinding& Binding : Descriptor.TextureBindings)
				{
					const FName Name = *Names[Binding.MaterialRole];
					if (!Material->FindParameterDefinition(Name)) continue;
					FPreparedSceneOutput* Texture = FindOutput(Binding.TextureIdentity);
					if (!Texture || !Cast<DTexture2D>(Texture->Candidate))
					{
						Abandon(Prepared);
						return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
							"scene-dependency-binding", "Scene texture dependency is unavailable.",
							Descriptor.StableIdentity);
					}
					(void)Material->SetTextureParameterValue(
						Name, Cast<DTexture2D>(Texture->Candidate));
				}
			}
			else if (Descriptor.Kind == ESceneOutputKind::StaticMesh)
			{
				auto* Mesh = Cast<DStaticMesh>(Output.Candidate);
				for (const FSceneOutputData& Candidate : Data->Outputs)
					if (Candidate.Kind == ESceneOutputKind::MaterialInstance)
					{
						FPreparedSceneOutput* Material = FindOutput(Candidate.StableIdentity);
						if (!Material || !Mesh->SetImportedDefaultMaterial(Candidate.SourceIndex,
							Cast<DMaterialInstance>(Material->Candidate), Error))
						{
							Abandon(Prepared);
							return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
								"scene-dependency-binding", Error.empty()
									? "Scene material dependency is unavailable." : std::move(Error),
								Descriptor.StableIdentity);
						}
					}
			}
			else if (Descriptor.Kind == ESceneOutputKind::SkeletalMesh)
			{
				const FImportedSkeletalMeshData& Imported =
					Data->Scene.SkeletalMeshes[Descriptor.SourceIndex];
				const FImportedSkeletonData& Skeleton =
					Data->Scene.Skeletons[Imported.SkeletonIndex];
				std::vector<FMeshMaterialSlotDefinition> Slots = Imported.MaterialSlots;
				for (auto& Slot : Slots)
					if (!(Slot.DefaultMaterial = FindMaterial(Slot.SourceMaterialIndex)))
					{
						Abandon(Prepared);
						return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
							"scene-dependency-binding", "Scene material dependency is unavailable.",
							Descriptor.StableIdentity);
					}
				FPreparedSceneOutput* SkeletonOutput = FindOutput(Descriptor.SkeletonIdentity);
				if (!SkeletonOutput || !Cast<DSkeletalMesh>(Output.Candidate)->PublishBuiltProduct({
					.Skeleton = SkeletonOutput ? Cast<DSkeleton>(SkeletonOutput->Candidate) : nullptr,
					.ValidationSkeleton = SkeletonOutput ? Cast<DSkeleton>(SkeletonOutput->Candidate) : nullptr,
					.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
					.MeshNodeBindTransform = Output.SkeletalMesh.MeshNodeBindTransform,
					.MaterialSlots = std::move(Slots), .Payload = std::move(Output.SkeletalMesh.Payload),
					.DerivedDataKey = std::move(Output.SkeletalMesh.DerivedDataKey),
					.DiagnosticMessage = std::move(Output.SkeletalMesh.Diagnostic),
					.bLoadedFromDerivedDataCache =
						Output.SkeletalMesh.bLoadedFromDerivedDataCache}, Error))
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
						"scene-dependency-binding", std::move(Error), Descriptor.StableIdentity);
				}
			}
			else if (Descriptor.Kind == ESceneOutputKind::AnimationClip)
			{
				const FImportedAnimationClipData& Imported =
					Data->Scene.AnimationClips[Descriptor.SourceIndex];
				const FImportedSkeletonData& Skeleton =
					Data->Scene.Skeletons[Imported.SkeletonIndex];
				FPreparedSceneOutput* SkeletonOutput = FindOutput(Descriptor.SkeletonIdentity);
				if (!SkeletonOutput || !Cast<DAnimationClip>(Output.Candidate)->PublishBuiltProduct({
					.Skeleton = SkeletonOutput ? Cast<DSkeleton>(SkeletonOutput->Candidate) : nullptr,
					.ValidationSkeleton = SkeletonOutput ? Cast<DSkeleton>(SkeletonOutput->Candidate) : nullptr,
					.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
					.ClipName = Output.Animation.ClipName,
					.Payload = std::move(Output.Animation.Payload),
					.DerivedDataKey = std::move(Output.Animation.DerivedDataKey),
					.DiagnosticMessage = std::move(Output.Animation.Diagnostic),
					.bLoadedFromDerivedDataCache =
						Output.Animation.bLoadedFromDerivedDataCache}, Error))
				{
					Abandon(Prepared);
					return AddError(OutResult, EImportDiagnosticCategory::MissingDependency,
						"scene-dependency-binding", std::move(Error), Descriptor.StableIdentity);
				}
			}
			if (!ValidateCandidate(Output, Error))
			{
				Abandon(Prepared);
				return AddError(OutResult, EImportDiagnosticCategory::ValidationFailure,
					"scene-validation", Error.empty()
						? "Scene candidate has no validated runtime data." : std::move(Error),
					Descriptor.StableIdentity);
			}
		}

		std::vector<DPackage*> Packages;
		Packages.reserve(Prepared.size());
		for (const FPreparedSceneOutput& Output : Prepared) Packages.push_back(Output.Package);
		Asset::FAssetBundleSaveOptions SaveOptions;
		if (!Packages.empty()) SaveOptions.RootPackage = Packages.back();
		const Asset::FAssetResult Saved = Asset::SavePackagesAtomically(Packages, SaveOptions);
		OutResult.bSucceeded = true;
		OutResult.bPersisted = Saved.Succeeded();
		if (!Saved)
		{
			OutResult.Message = Saved.Message;
			OutResult.Diagnostics.push_back({
				.Severity = EImportDiagnosticSeverity::Warning,
				.Category = EImportDiagnosticCategory::PersistenceFailure,
				.Phase = "scene-persistence",
				.Message = Saved.Message});
		}
		else OutResult.Message.clear();
		return true;
	}
}
