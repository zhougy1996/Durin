#include "SceneImportProviderSchema.h"
#include "BuiltinImportProviderCommon.h"
#include "BuiltinProviderRegistration.h"
#include "DObject/Package.h"
#include "Asset/AssetOperations.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		class FStaticMeshExchange final : public IPreparedImportedStateExchange
		{
		public:
			explicit FStaticMeshExchange(
				std::unique_ptr<FStaticMeshImportedStateExchange> InExchange)
				: Exchange(std::move(InExchange)) {}
			auto Commit() noexcept -> void override { Exchange->Commit(); }
			auto Reverse() noexcept -> void override { Exchange->Reverse(); }
			auto Finalize() noexcept -> void override { Exchange->Finalize(); }

		private:
			std::unique_ptr<FStaticMeshImportedStateExchange> Exchange;
		};

		auto CloneStaticMeshBuildProduct(
			const FStaticMeshBuildProduct& Source)
			-> FStaticMeshBuildProduct
		{
			FStaticMeshBuildProduct Result;
			if (Source.RenderData)
				Result.RenderData =
					std::make_unique<FStaticMeshRenderData>(*Source.RenderData);
			Result.MaterialSlots = Source.MaterialSlots;
			Result.SourceImportData = Source.SourceImportData;
			Result.NormalizedSize = Source.NormalizedSize;
			Result.DerivedDataKey = Source.DerivedDataKey;
			Result.bSlotMetadataChanged = Source.bSlotMetadataChanged;
			Result.DerivedDataStatus = Source.DerivedDataStatus;
			Result.DiagnosticMessage = Source.DiagnosticMessage;
			Result.bSourceImporterInvoked = Source.bSourceImporterInvoked;
			Result.bMarkPackageDirty = Source.bMarkPackageDirty;
			Result.FailureStage = Source.FailureStage;
			return Result;
		}

		class FStaticMeshReconciliationContext final
			: public IReconciliationContext
		{
		public:
			Asset::FStaticMeshReconciliationSnapshot Snapshot;
		};

	class FSceneSourceTranslator final : public ISourceTranslator
		{
		public:
			auto Recognize(const FImportSourceRecognition& Source) const -> bool override
			{
				std::string Extension(Source.Extension);
				std::ranges::transform(Extension, Extension.begin(), [](unsigned char Value) {
					return static_cast<char>(std::tolower(Value)); });
				return Extension == ".gltf" || Extension == ".glb" || Extension == ".fbx"
					|| Extension == ".obj" || Extension == ".dae" || Extension == ".3ds"
					|| Extension == ".ply" || Extension == ".stl";
			}
			auto DiscoverDependencies(
				std::span<const FSourceSnapshotEntry> Sources,
				FDependencyRequestSink& Sink,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				return DiscoverSceneImportDependencies(Sources, Sink, OutDiagnostics);
			}
			auto Translate(
				const FSourceSnapshot& Snapshot,
				const FSchemaPayload& Settings,
				FSourceGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				FSceneImportPlan Request;
				std::string Error;
				if (!DecodeSceneImportPlan(Settings, Request, Error))
				{
					OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::InvalidPlan,
						.Identity = "Durin.Scene.InvalidTranslationSettings",
						.Phase = "Translation", .Message = std::move(Error)});
					return false;
				}
				std::shared_ptr<const FSceneProviderPlanData> Data;
				std::vector<FImportOutputPreview> Outputs;
				if (!BuildSceneImportPlanData(Snapshot, Request.DestinationDirectory,
					Request.MeshSettings, Data, Outputs, OutDiagnostics, Error))
				{
					OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::ProviderFailure,
						.Identity = "Durin.Scene.DecodeFailed", .Phase = "Translation",
						.SourceIdentity = "root", .Message = std::move(Error)});
					return false;
				}
				auto MutableData = std::make_shared<FSceneProviderPlanData>(*Data);
				MutableData->Outputs.push_back({.StableIdentity = "scene-import-record",
					.Kind = ESceneOutputKind::ImportRecord});
				const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
				const std::string RecordName = Root
					? std::filesystem::path(Root->SourcePath.Path).stem().generic_string() + "_Import"
					: "Scene_Import";
				FAssetPath RecordPath;
				if (!FAssetPath::TryCreate(std::format("{}/{}",
					Request.DestinationDirectory.ToString(), RecordName), RecordPath, &Error))
					return false;
				Outputs.push_back({.StableIdentity = "scene-import-record", .Role = "ImportRecord",
					.AssetPath = RecordPath, .AssetClassName = "Durin::AssetForge::DImportRecord",
					.Policy = EImportOutputPolicy::Create, .Collision = EImportCollisionAction::Create});
				Data = std::move(MutableData);
				std::vector<std::byte> KeyBytes;
				AppendValue(KeyBytes, Request.MeshSettings.ForwardAxis);
				AppendValue(KeyBytes, Request.MeshSettings.RightAxis);
				AppendValue(KeyBytes, Request.MeshSettings.UpAxis);
				for (const FSourceSnapshotEntry& Source : Snapshot.GetSources())
				{
					AppendString(KeyBytes, Source.StableIdentity);
					AppendValue(KeyBytes, Source.ContentHash.HashLow);
					AppendValue(KeyBytes, Source.ContentHash.HashHigh);
				}
				const std::string CacheKey = FXxHash128::HashBuffer(KeyBytes).ToString();
				auto Cached = std::make_shared<FSceneCachedImportPlan>();
				Cached->Data = std::move(Data);
				Cached->Snapshot = std::make_shared<FSourceSnapshot>(Snapshot);
				Cached->Outputs = std::move(Outputs);
				{
					std::lock_guard Lock(GSceneImportCacheMutex);
					GSceneImportCache[CacheKey] = Cached;
					GSceneImportOutputCache[
						MakeSceneOutputCacheKey(CacheKey, Request.DestinationDirectory)] = Cached->Outputs;
				}
				std::unordered_map<uint32, std::string> MaterialIdentities;
				for (const FSceneOutputData& Output : Cached->Data->Outputs)
					if (Output.Kind == ESceneOutputKind::MaterialInstance)
						MaterialIdentities.emplace(Output.SourceIndex, Output.StableIdentity);
				for (uint32 Index = 0; Index < Cached->Data->Outputs.size(); ++Index)
				{
					const FSceneOutputData& Output = Cached->Data->Outputs[Index];
					std::vector<std::string> Dependencies;
					if (Output.Kind == ESceneOutputKind::MaterialInstance)
						for (const FSceneMaterialTextureBinding& Binding : Output.TextureBindings)
							Dependencies.push_back(Binding.TextureIdentity);
					else if (Output.Kind == ESceneOutputKind::SkeletalMesh
						|| Output.Kind == ESceneOutputKind::AnimationClip)
						Dependencies.push_back(Output.SkeletonIdentity);
					if (Output.Kind == ESceneOutputKind::StaticMesh
						|| Output.Kind == ESceneOutputKind::SkeletalMesh)
						for (const auto& [_, Identity] : MaterialIdentities)
							Dependencies.push_back(Identity);
					if (Output.Kind == ESceneOutputKind::ImportRecord)
						for (const FSceneOutputData& Dependency : Cached->Data->Outputs)
							if (Dependency.Kind != ESceneOutputKind::ImportRecord)
								Dependencies.push_back(Dependency.StableIdentity);
					std::vector<std::string> SourceIdentities{"root"};
					if (Output.Kind == ESceneOutputKind::Texture2D
						&& Output.SourceIndex < Cached->Data->Scene.Images.size())
					{
						const FImportedImage& Image = Cached->Data->Scene.Images[Output.SourceIndex];
						if (Image.ExternalDependencyIndex
							&& *Image.ExternalDependencyIndex < Cached->Data->Scene.Dependencies.size())
							SourceIdentities.push_back(Cached->Data->Scene.Dependencies[
								*Image.ExternalDependencyIndex].StableIdentity);
					}
					if (!Builder.AddNode({.StableIdentity = Output.StableIdentity,
						.NodeKind = std::string(SceneNodeKind(Output.Kind)),
						.Payload = EncodeSceneNodeReference(CacheKey, Index),
						.SourceIdentities = std::move(SourceIdentities),
						.Dependencies = std::move(Dependencies)})) return false;
				}
				return true;
			}
		};

		class FDefaultScenePlanningPass final : public IPlanningPass
		{
		public:
			auto Execute(
				const FSourceGraph& SourceGraph,
				const FBuildGraph*,
				const FSchemaPayload& Settings,
				FBuildGraphBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				FSceneImportPlan Request;
				std::string Error;
				if (!DecodeSceneImportPlan(Settings, Request, Error)) return false;
				for (const FSourceNode& Node : SourceGraph.GetNodes())
				{
					std::shared_ptr<const FSceneCachedImportPlan> Cached;
					const FSceneOutputData* Output = nullptr;
					if (!DecodeSceneNodeReference(Node.Payload, Cached, Output, Error))
					{
						OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
							.Category = EImportDiagnosticCategory::InvalidPlan,
							.Identity = "Durin.Scene.StaleNode", .Phase = "PlanningPass",
							.OutputIdentity = Node.StableIdentity, .Message = std::move(Error)});
						return false;
					}
					std::vector<FImportOutputPreview> Outputs;
					std::span<const std::byte> ReferenceBytes(Node.Payload.Bytes);
					std::string SourceKey;
					uint32 IgnoredIndex = 0;
					if (!ReadString(ReferenceBytes, SourceKey) || !ReadValue(ReferenceBytes, IgnoredIndex))
						return false;
					{
						std::lock_guard Lock(GSceneImportCacheMutex);
						const auto It = GSceneImportOutputCache.find(
							MakeSceneOutputCacheKey(SourceKey, Request.DestinationDirectory));
						if (It == GSceneImportOutputCache.end()) return false;
						Outputs = It->second;
					}
					const auto Preview = std::ranges::find(
						Outputs, Output->StableIdentity, &FImportOutputPreview::StableIdentity);
					if (Preview == Outputs.end()) return false;
					const auto Existing = std::ranges::find(Request.ExistingMappings,
						Output->StableIdentity, &FOutputMapping::OutputIdentity);
					const FAssetPath OutputPath = Existing == Request.ExistingMappings.end()
						? Preview->AssetPath : Existing->AssetPath;
					Preview->AssetPath = OutputPath;
					Preview->Policy = Existing == Request.ExistingMappings.end()
						? Preview->Policy : EImportOutputPolicy::ReplaceWholeState;
					{
						std::lock_guard Lock(GSceneImportCacheMutex);
						GSceneImportOutputCache[
							MakeSceneOutputCacheKey(SourceKey, Request.DestinationDirectory)] = Outputs;
					}
					if (!Builder.AddNode({.StableIdentity = Output->StableIdentity,
						.BuilderId = std::string(SceneBuilderIds[SceneAssetBuilderIndex(Output->Kind)]),
						.BuilderContractVersion = 1,
						.OutputClassName = std::string(SceneOutputClassName(Output->Kind)),
						.Destination = OutputPath,
						.Policy = Existing == Request.ExistingMappings.end()
							? Preview->Policy : EImportOutputPolicy::ReplaceWholeState,
						.Settings = Settings,
						.SourceNodeReferences = {Output->StableIdentity},
						.BuildDependencies = Node.Dependencies})) return false;
				}
				for (const FOutputMapping& Existing : Request.ExistingMappings)
					if (!SourceGraph.FindNode(Existing.OutputIdentity))
						OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Warning,
							.Category = EImportDiagnosticCategory::StalePlan,
							.Identity = "Durin.Scene.RemovedOutput", .Phase = "PlanningPass",
							.OutputIdentity = Existing.OutputIdentity,
							.Message = "Previously managed Scene output is absent and remains orphaned."});
				return true;
			}
		};

		class FSceneBuildProduct final : public IBuildProduct
		{
		public:
			std::shared_ptr<const FSceneCachedImportPlan> Cached;
			uint32 OutputIndex = 0;
			std::vector<FImportOutputPreview> Outputs;
			FStaticMeshBuildProduct StaticMesh;
			FSceneTextureBuildProduct Texture;
			Asset::FSkeletalMeshBuildProduct SkeletalMesh;
			Asset::FAnimationClipBuildProduct Animation;
			auto CloneDetachedProduct() const
				-> std::unique_ptr<IBuildProduct> override
			{
				auto Result = std::make_unique<FSceneBuildProduct>();
				Result->Cached = Cached;
				Result->OutputIndex = OutputIndex;
				Result->Outputs = Outputs;
				Result->StaticMesh = CloneStaticMeshBuildProduct(StaticMesh);
				Result->Texture = Texture;
				Result->SkeletalMesh = SkeletalMesh;
				Result->Animation = Animation;
				return Result;
			}
		};

		template<typename TExchange>
		class TOwnedSceneExchange final : public IPreparedImportedStateExchange
		{
		public:
			explicit TOwnedSceneExchange(std::unique_ptr<TExchange> InExchange)
				: Exchange(std::move(InExchange)) {}
			auto Commit() noexcept -> void override { Exchange->Commit(); }
			auto Reverse() noexcept -> void override { Exchange->Reverse(); }
			auto Finalize() noexcept -> void override { Exchange->Finalize(); }
		private:
			std::unique_ptr<TExchange> Exchange;
		};

			class FSceneAssetBuilder final : public IAssetBuilder
		{
			public:
				explicit FSceneAssetBuilder(ESceneOutputKind InKind) : Kind(InKind) {}

				auto LoadExistingTarget(
					const FBuildNode& AssetBuilderNode,
					DObject*& OutTarget) const -> Asset::FAssetResult override
				{
					const FScopedSkeletalDerivedDataRepairLoad RepairLoad;
					return Asset::LoadAsset(AssetBuilderNode.Destination, OutTarget);
				}

			auto BuildDetachedProduct(
				const FBuildNode& AssetBuilderNode,
				const FSourceGraph& SourceGraph,
				IImportProgressReporter*,
				const std::function<bool()>& IsCancellationRequested,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<IBuildProduct> override
			{
				if (IsCancellationRequested()) return {};
				const FSourceNode* Node = AssetBuilderNode.SourceNodeReferences.empty()
					? nullptr : SourceGraph.FindNode(AssetBuilderNode.SourceNodeReferences.front());
				std::shared_ptr<const FSceneCachedImportPlan> Cached;
				const FSceneOutputData* Output = nullptr;
				std::string Error;
				if (!Node || !DecodeSceneNodeReference(Node->Payload, Cached, Output, Error)
					|| Output->Kind != Kind)
				{
					OutDiagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Identity = "Durin.Scene.FactoryPayloadInvalid", .Phase = "ProductBuild",
						.OutputIdentity = AssetBuilderNode.StableIdentity, .Message = std::move(Error)});
					return {};
				}
				auto Product = std::make_unique<FSceneBuildProduct>();
				Product->Cached = Cached;
				Product->OutputIndex = static_cast<uint32>(Output - Cached->Data->Outputs.data());
				FSceneImportPlan AssetBuilderPlan;
				if (!DecodeSceneImportPlan(AssetBuilderNode.Settings, AssetBuilderPlan, Error))
					return Fail(AssetBuilderNode, std::move(Error), OutDiagnostics);
				const FSchemaPayload AuthoredSettings =
					EncodeSceneAuthoredSettings(AssetBuilderPlan);
				std::span<const std::byte> ReferenceBytes(Node->Payload.Bytes);
				std::string SourceKey;
				uint32 IgnoredIndex = 0;
				if (!ReadString(ReferenceBytes, SourceKey) || !ReadValue(ReferenceBytes, IgnoredIndex))
					return Fail(AssetBuilderNode, "Scene source-node reference is malformed.", OutDiagnostics);
				{
					std::lock_guard Lock(GSceneImportCacheMutex);
					const auto It = GSceneImportOutputCache.find(
						MakeSceneOutputCacheKey(SourceKey, AssetBuilderPlan.DestinationDirectory));
					if (It == GSceneImportOutputCache.end())
						return Fail(AssetBuilderNode, "Scene output plan is unavailable.", OutDiagnostics);
					Product->Outputs = It->second;
				}
				const FSourceSnapshotEntry* Root = Cached->Snapshot->FindSource("root");
				if (Kind == ESceneOutputKind::Texture2D)
				{
					if (!BuildSceneImportTextureProduct(*Cached->Snapshot, *Cached->Data,
						*Output, IsCancellationRequested, Product->Texture, Error)) return Fail(
						AssetBuilderNode, std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::StaticMesh)
				{
					if (!Root) return Fail(AssetBuilderNode, "Scene root source is unavailable.", OutDiagnostics);
					FStaticMeshSourceImportData Provenance{
						.SourcePath = Root->SourcePath,
						.SourceContentHash = Root->ContentHash.ToString(),
						.ImporterId = std::string(SceneTranslatorId), .ImporterVersion = 1,
						.ImportSettings = Cached->Data->MeshSettings};
					Asset::FStaticMeshReconciliationSnapshot Reconciliation{
						.StableObjectPath = AssetBuilderNode.Destination.ToString(),
						.Provenance = Provenance, .ImportSettings = Cached->Data->MeshSettings};
					if (AssetBuilderNode.Policy == EImportOutputPolicy::Create
						&& !Asset::FStaticMeshBuildOperations::BuildImportedProduct(
						Reconciliation, MakeStaticMeshImportedData(Cached->Data->Scene),
						std::move(Provenance), Root->SourcePath.Path, Product->StaticMesh, Error))
						return Fail(AssetBuilderNode, std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::SkeletalMesh)
				{
					if (Output->SourceIndex >= Cached->Data->Scene.SkeletalMeshes.size())
						return Fail(AssetBuilderNode, "Scene SkeletalMesh mapping is invalid.", OutDiagnostics);
					const FImportedSkeletalMeshData& Imported =
						Cached->Data->Scene.SkeletalMeshes[Output->SourceIndex];
					if (Imported.SkeletonIndex >= Cached->Data->Scene.Skeletons.size())
						return Fail(AssetBuilderNode, "Scene Skeleton mapping is invalid.", OutDiagnostics);
					const FImportedSkeletonData& Skeleton = Cached->Data->Scene.Skeletons[Imported.SkeletonIndex];
					Asset::FSkeletalMeshBuildKeyInput Key;
					static_cast<Asset::FSkeletalBuildKeyFields&>(Key) = {
						.ProviderIdentity = std::string(SceneTranslatorId), .ProviderVersion = 1,
						.SourceClosureHash = Root ? Root->ContentHash : FXxHash128{},
						.SettingsHash = AuthoredSettings.ContentHash,
						.ProviderStateHash = Node->Payload.ContentHash,
						.StableOutputIdentity = Output->StableIdentity,
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
						.TargetProfile = ESkeletalPayloadTargetProfile::Game};
					if (!Asset::BuildSkeletalMeshProduct({
						.SkeletonBoneCount = static_cast<uint32>(Skeleton.Bones.size()),
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.MeshNodeBindTransform = Imported.MeshNodeBindTransform,
						.MaterialSlotCount = static_cast<uint32>(Imported.MaterialSlots.size()),
						.Payload = Imported.Payload, .KeyInput = std::move(Key)},
						Product->SkeletalMesh, Error))
						return Fail(AssetBuilderNode, std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::AnimationClip)
				{
					if (Output->SourceIndex >= Cached->Data->Scene.AnimationClips.size())
						return Fail(AssetBuilderNode, "Scene AnimationClip mapping is invalid.", OutDiagnostics);
					const FImportedAnimationClipData& Imported =
						Cached->Data->Scene.AnimationClips[Output->SourceIndex];
					if (Imported.SkeletonIndex >= Cached->Data->Scene.Skeletons.size())
						return Fail(AssetBuilderNode, "Scene Skeleton mapping is invalid.", OutDiagnostics);
					const FImportedSkeletonData& Skeleton = Cached->Data->Scene.Skeletons[Imported.SkeletonIndex];
					Asset::FAnimationClipBuildKeyInput Key;
					static_cast<Asset::FSkeletalBuildKeyFields&>(Key) = {
						.ProviderIdentity = std::string(SceneTranslatorId), .ProviderVersion = 1,
						.SourceClosureHash = Root ? Root->ContentHash : FXxHash128{},
						.SettingsHash = AuthoredSettings.ContentHash,
						.ProviderStateHash = Node->Payload.ContentHash,
						.StableOutputIdentity = Output->StableIdentity,
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
						.TargetProfile = ESkeletalPayloadTargetProfile::Game};
					if (!Asset::BuildAnimationClipProduct({
						.SkeletonBoneCount = static_cast<uint32>(Skeleton.Bones.size()),
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.ClipName = FName(Imported.SuggestedName), .Payload = Imported.Payload,
						.KeyInput = std::move(Key)}, Product->Animation, Error))
						return Fail(AssetBuilderNode, std::move(Error), OutDiagnostics);
				}
				return Product;
			}

			auto CaptureReconciliationContext(
				const FBuildNode& AssetBuilderNode,
				const DObject& ExistingTarget,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<IReconciliationContext> override
			{
				if (Kind != ESceneOutputKind::StaticMesh) return {};
				const auto* Mesh = Cast<DStaticMesh>(&ExistingTarget);
				if (!Mesh)
				{
					(void)Fail(AssetBuilderNode,
						"Scene StaticMesh replacement target is invalid.", OutDiagnostics);
					return {};
				}
				auto Result = std::make_unique<FStaticMeshReconciliationContext>();
				Result->Snapshot =
					Asset::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(*Mesh);
				return Result;
			}

			auto ReconcileDetachedProduct(
				const FBuildNode& AssetBuilderNode,
				const IReconciliationContext* Context,
				IBuildProduct& ProductObject,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				if (Kind != ESceneOutputKind::StaticMesh
					|| AssetBuilderNode.Policy == EImportOutputPolicy::Create) return true;
				const auto* Reconciliation =
					dynamic_cast<const FStaticMeshReconciliationContext*>(Context);
				auto* Product = dynamic_cast<FSceneBuildProduct*>(&ProductObject);
				const FSourceSnapshotEntry* Root = Product && Product->Cached
					? Product->Cached->Snapshot->FindSource("root") : nullptr;
				std::string Error;
				if (!Reconciliation || !Product || !Root
					|| Product->OutputIndex >= Product->Cached->Data->Outputs.size())
				{
					(void)Fail(AssetBuilderNode,
						"Scene StaticMesh replacement context is invalid.", OutDiagnostics);
					return false;
				}
				FStaticMeshSourceImportData Provenance{
					.SourcePath = Root->SourcePath,
					.SourceContentHash = Root->ContentHash.ToString(),
					.ImporterId = std::string(SceneTranslatorId), .ImporterVersion = 1,
					.ImportSettings = Product->Cached->Data->MeshSettings};
				if (!Asset::FStaticMeshBuildOperations::BuildImportedProduct(
					Reconciliation->Snapshot,
					MakeStaticMeshImportedData(Product->Cached->Data->Scene),
					std::move(Provenance), Root->SourcePath.Path,
					Product->StaticMesh, Error))
				{
					(void)Fail(AssetBuilderNode, std::move(Error), OutDiagnostics);
					return false;
				}
				return true;
			}

			auto MaterializeCandidate(
				const FBuildNode& AssetBuilderNode,
				std::unique_ptr<IBuildProduct> InProduct,
				std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto Product = std::unique_ptr<FSceneBuildProduct>(
					dynamic_cast<FSceneBuildProduct*>(InProduct.release()));
				if (!Product || Product->OutputIndex >= Product->Cached->Data->Outputs.size()) return {};
				FAssetPath CandidatePath = AssetBuilderNode.Destination;
				if (AssetBuilderNode.Policy != EImportOutputPolicy::Create
					&& !MakeCandidatePath(AssetBuilderNode.Destination, CandidatePath)) return {};
				DObject* Candidate = nullptr;
				Asset::FAssetResult Created;
				if (Kind == ESceneOutputKind::StaticMesh) { DStaticMesh* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::MaterialInstance) { DMaterialInstance* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::Skeleton) { DSkeleton* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::SkeletalMesh) { DSkeletalMesh* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::AnimationClip) { DAnimationClip* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else if (Kind == ESceneOutputKind::Texture2D) { DTexture2D* Value = nullptr; Created = Asset::CreateAsset(CandidatePath, Value); Candidate = Value; }
				else { AssetForge::DImportRecord* Value = nullptr; Created = AssetForge::CreateImportRecordAsset(CandidatePath, Value); Candidate = Value; }
				if (!Created || !Candidate) return {};
				auto Result = std::make_unique<FBuiltinSingleAssetCandidate>(
					Candidate, AssetBuilderNode.Policy == EImportOutputPolicy::Create);
				std::string Error;
				const FSceneOutputData& Output = Product->Cached->Data->Outputs[Product->OutputIndex];
				if (Kind == ESceneOutputKind::Skeleton)
				{
					if (Output.SourceIndex >= Product->Cached->Data->Scene.Skeletons.size()
						|| !Cast<DSkeleton>(Candidate)->InitializeCanonicalBones(
							Product->Cached->Data->Scene.Skeletons[Output.SourceIndex].Bones, Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::Texture2D)
				{
					if (!Asset::PublishTexture2DProduct(*Cast<DTexture2D>(Candidate),
						std::move(Product->Texture.Product), {.SourcePath = Product->Texture.Source,
							.DecoderId = "DurinImage", .DecoderVersion = 1,
							.SourceFileSize = Product->Texture.SourceFileSize}, Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::StaticMesh)
				{
					if (!Asset::FStaticMeshBuildOperations::PublishImportedProduct(
						*Cast<DStaticMesh>(Candidate), std::move(Product->StaticMesh), Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
				}
				else if (Kind == ESceneOutputKind::ImportRecord)
				{
					FSceneImportPlan PersistedPlan;
					if (!DecodeSceneImportPlan(
						AssetBuilderNode.Settings, PersistedPlan, Error))
						return MaterializationFailure(
							std::move(Result), std::move(Error), OutDiagnostics);
					const FSchemaPayload AuthoredSettings =
						EncodeSceneAuthoredSettings(PersistedPlan);
					AssetForge::FImportRecordState State{
						.ProviderId = std::string(SceneTranslatorId), .ProviderContractVersion = 1};
					if (!AssetForge::MakeImportRecordPayload(std::string(ScenePlanSchema), 1,
						AuthoredSettings.Bytes, AssetForge::MaximumImportRecordSettingsBytes,
						State.Settings, Error)
						|| !AssetForge::MakeImportRecordPayload("Durin.AssetForge.Scene.State", 2,
							AuthoredSettings.Bytes, AssetForge::MaximumImportRecordProviderStateBytes,
							State.ProviderState, Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
					for (const FSourceSnapshotEntry& Source : Product->Cached->Snapshot->GetSources())
						State.Sources.push_back({.StableIdentity = Source.StableIdentity,
							.Role = Source.Role, .SourcePath = Source.SourcePath,
							.ContentHashLow = Source.ContentHash.HashLow,
							.ContentHashHigh = Source.ContentHash.HashHigh,
							.ByteCount = Source.ByteCount});
					for (const FImportOutputPreview& Preview : Product->Outputs)
					{
						if (Preview.StableIdentity == "scene-import-record") continue;
						std::vector<std::byte> FingerprintBytes;
						AppendString(FingerprintBytes, Preview.StableIdentity);
						AppendValue(FingerprintBytes, AuthoredSettings.ContentHash.HashLow);
						AppendValue(FingerprintBytes, AuthoredSettings.ContentHash.HashHigh);
						State.Outputs.push_back({.StableIdentity = Preview.StableIdentity,
							.Role = Preview.Role, .AssetPath = Preview.AssetPath,
							.AssetClassName = Preview.AssetClassName,
							.Policy = AssetForge::EImportRecordOutputPolicy::Managed,
							.AuthoredFingerprint = FXxHash128::HashBuffer(FingerprintBytes).ToString()});
					}
					if (!Cast<AssetForge::DImportRecord>(Candidate)->SetState(std::move(State), Error))
						return MaterializationFailure(std::move(Result), std::move(Error), OutDiagnostics);
				}
				if (Kind == ESceneOutputKind::MaterialInstance
					|| Kind == ESceneOutputKind::StaticMesh
					|| Kind == ESceneOutputKind::SkeletalMesh
					|| Kind == ESceneOutputKind::AnimationClip)
				{
					std::lock_guard Lock(PendingMutex);
					Pending.emplace(Candidate, std::move(Product));
				}
				return Result;
			}

			auto ResolveCandidateDependencies(
				const FBuildNode& AssetBuilderNode,
				ISingleAssetCandidate& Candidate,
				const FMaterializationContext& Context,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				if (Kind == ESceneOutputKind::Skeleton || Kind == ESceneOutputKind::Texture2D
					|| Kind == ESceneOutputKind::ImportRecord)
					return true;
				std::unique_ptr<FSceneBuildProduct> Product;
				{
					std::lock_guard Lock(PendingMutex);
					auto It = Pending.find(Candidate.GetAsset());
					if (It == Pending.end()) return false;
					Product = std::move(It->second);
					Pending.erase(It);
				}
				const FSceneOutputData& Output = Product->Cached->Data->Outputs[Product->OutputIndex];
				std::string Error;
				auto FindMaterial = [&](uint32 SourceIndex) -> DMaterialInterface* {
					for (const FSceneOutputData& Descriptor : Product->Cached->Data->Outputs)
						if (Descriptor.Kind == ESceneOutputKind::MaterialInstance
							&& Descriptor.SourceIndex == SourceIndex)
							return Cast<DMaterialInterface>(Context.ExistingTarget(Descriptor.StableIdentity));
					return nullptr;
				};
				if (Kind == ESceneOutputKind::MaterialInstance)
				{
					DMaterial* Standard = nullptr;
					FAssetPath StandardPath;
					const auto Imported = std::ranges::find(Product->Cached->Data->Scene.Materials,
						Output.SourceIndex, &FImportedMaterial::SourceMaterialIndex);
					if (!FAssetPath::TryCreate(ImportedSurfaceMaterialPath, StandardPath, &Error)
						|| !Asset::LoadAsset(StandardPath, Standard) || !Standard
						|| Imported == Product->Cached->Data->Scene.Materials.end()) return false;
					auto* Material = Cast<DMaterialInstance>(Candidate.GetAsset());
					FMaterialStaticProperties StaticProperties = Standard->GetStaticProperties();
					StaticProperties.BlendMode = Imported->AlphaMode == EImportedAlphaMode::Mask
						? EMaterialBlendMode::Masked : Imported->AlphaMode == EImportedAlphaMode::Blend
							? EMaterialBlendMode::Translucent : EMaterialBlendMode::Opaque;
					StaticProperties.bTwoSided = Imported->bDoubleSided;
					StaticProperties.OpacityMaskThreshold = Imported->AlphaCutoff;
					if (!Material || !Material->SetParent(Standard)
						|| !Material->SetStaticPropertiesOverride(StaticProperties)
						|| !Material->SetVectorParameterValue(MaterialParameters::BaseColorName(), FVector3(Imported->BaseColorFactor))
						|| !Material->SetScalarParameterValue(MaterialParameters::OpacityName(), Imported->BaseColorFactor.a)
						|| !Material->SetScalarParameterValue(MaterialParameters::MetallicName(), Imported->MetallicFactor)
						|| !Material->SetScalarParameterValue(MaterialParameters::RoughnessName(), Imported->RoughnessFactor)) return false;
					const std::array<const FName*, 8> Names{&MaterialParameters::BaseColorTextureName(),
						&MaterialParameters::NormalTextureName(), &MaterialParameters::MetallicTextureName(),
						&MaterialParameters::RoughnessTextureName(), &MaterialParameters::AmbientOcclusionTextureName(),
						&MaterialParameters::EmissiveTextureName(), &MaterialParameters::OpacityTextureName(),
						&MaterialParameters::OpacityMaskTextureName()};
					for (const FSceneMaterialTextureBinding& Binding : Output.TextureBindings)
						if (!Material->SetTextureParameterValue(*Names[Binding.MaterialRole],
							Cast<DTexture2D>(Context.ExistingTarget(Binding.TextureIdentity)))) return false;
				}
				else if (Kind == ESceneOutputKind::StaticMesh)
				{
					auto* Mesh = Cast<DStaticMesh>(Candidate.GetAsset());
					for (const FSceneOutputData& Descriptor : Product->Cached->Data->Outputs)
						if (Descriptor.Kind == ESceneOutputKind::MaterialInstance
							&& !Mesh->SetImportedDefaultMaterial(Descriptor.SourceIndex,
								Cast<DMaterialInstance>(Context.ExistingTarget(Descriptor.StableIdentity)), Error))
							return false;
				}
				else if (Kind == ESceneOutputKind::SkeletalMesh)
				{
					const FImportedSkeletalMeshData& Imported = Product->Cached->Data->Scene.SkeletalMeshes[Output.SourceIndex];
					const FImportedSkeletonData& Skeleton = Product->Cached->Data->Scene.Skeletons[Imported.SkeletonIndex];
					std::vector<FMeshMaterialSlotDefinition> Slots = Imported.MaterialSlots;
					for (auto& Slot : Slots) if (!(Slot.DefaultMaterial = FindMaterial(Slot.SourceMaterialIndex))) return false;
					if (!Cast<DSkeletalMesh>(Candidate.GetAsset())->PublishBuiltProduct({
						.Skeleton = Cast<DSkeleton>(Context.ExistingTarget(Output.SkeletonIdentity)),
						.ValidationSkeleton = Cast<DSkeleton>(Context.ProspectiveObject(Output.SkeletonIdentity)),
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.MeshNodeBindTransform = Product->SkeletalMesh.MeshNodeBindTransform,
						.MaterialSlots = std::move(Slots), .Payload = std::move(Product->SkeletalMesh.Payload),
						.DerivedDataKey = std::move(Product->SkeletalMesh.DerivedDataKey),
						.DiagnosticMessage = std::move(Product->SkeletalMesh.Diagnostic)}, Error)) return false;
				}
				else if (Kind == ESceneOutputKind::AnimationClip)
				{
					const FImportedAnimationClipData& Imported = Product->Cached->Data->Scene.AnimationClips[Output.SourceIndex];
					const FImportedSkeletonData& Skeleton = Product->Cached->Data->Scene.Skeletons[Imported.SkeletonIndex];
					if (!Cast<DAnimationClip>(Candidate.GetAsset())->PublishBuiltProduct({
						.Skeleton = Cast<DSkeleton>(Context.ExistingTarget(Output.SkeletonIdentity)),
						.ValidationSkeleton = Cast<DSkeleton>(Context.ProspectiveObject(Output.SkeletonIdentity)),
						.SkeletonCompatibilityIdentity = Skeleton.CompatibilityIdentity,
						.ClipName = Product->Animation.ClipName, .Payload = std::move(Product->Animation.Payload),
						.DerivedDataKey = std::move(Product->Animation.DerivedDataKey),
						.DiagnosticMessage = std::move(Product->Animation.Diagnostic)}, Error)) return false;
				}
				return true;
			}

			auto PrepareImportedStateExchange(DObject& Target, ISingleAssetCandidate& Candidate,
				std::vector<FImportDiagnostic>&) const -> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				std::string Error;
				if (Kind == ESceneOutputKind::StaticMesh)
				{
					auto Exchange = Cast<DStaticMesh>(&Target)->PrepareImportedStateExchange(
						*Cast<DStaticMesh>(Candidate.GetAsset()), Error);
					return Exchange ? std::make_unique<FStaticMeshExchange>(std::move(Exchange)) : nullptr;
				}
				if (Kind == ESceneOutputKind::Texture2D) return std::make_unique<TImportedStateExchange<DTexture2D>>(
					*Cast<DTexture2D>(&Target), *Cast<DTexture2D>(Candidate.GetAsset()));
				if (Kind == ESceneOutputKind::MaterialInstance) return std::make_unique<TImportedStateExchange<DMaterialInstance>>(
					*Cast<DMaterialInstance>(&Target), *Cast<DMaterialInstance>(Candidate.GetAsset()));
				if (Kind == ESceneOutputKind::ImportRecord) return std::make_unique<TImportedStateExchange<AssetForge::DImportRecord>>(
					*Cast<AssetForge::DImportRecord>(&Target), *Cast<AssetForge::DImportRecord>(Candidate.GetAsset()));
				if (Kind == ESceneOutputKind::Skeleton)
				{
					auto Exchange = Cast<DSkeleton>(&Target)->PrepareImportedStateExchange(*Cast<DSkeleton>(Candidate.GetAsset()), Error);
					return Exchange ? std::make_unique<TOwnedSceneExchange<FSkeletonImportedStateExchange>>(std::move(Exchange)) : nullptr;
				}
				if (Kind == ESceneOutputKind::SkeletalMesh)
				{
					auto* Mesh = Cast<DSkeletalMesh>(Candidate.GetAsset());
					auto Exchange = Cast<DSkeletalMesh>(&Target)->PrepareImportedStateExchange(*Mesh, *Mesh->GetSkeleton(), Error);
					return Exchange ? std::make_unique<TOwnedSceneExchange<FSkeletalMeshImportedStateExchange>>(std::move(Exchange)) : nullptr;
				}
				auto* Clip = Cast<DAnimationClip>(Candidate.GetAsset());
				auto Exchange = Cast<DAnimationClip>(&Target)->PrepareImportedStateExchange(*Clip, *Clip->GetSkeleton(), Error);
				return Exchange ? std::make_unique<TOwnedSceneExchange<FAnimationClipImportedStateExchange>>(std::move(Exchange)) : nullptr;
			}

		private:
			auto Fail(const FBuildNode& Node, std::string Message,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IBuildProduct>
			{
				Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::CandidateFailure,
					.Identity = "Durin.Scene.ProductBuildFailed", .Phase = "ProductBuild",
					.OutputIdentity = Node.StableIdentity, .Message = std::move(Message)});
				return {};
			}
			auto MaterializationFailure(std::unique_ptr<FBuiltinSingleAssetCandidate> Candidate,
				std::string Message, std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate>
			{
				Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::CandidateFailure,
					.Identity = "Durin.Scene.MaterializationFailed", .Phase = "Materialization",
					.Message = std::move(Message)});
				Candidate->Abandon();
				return {};
			}

			ESceneOutputKind Kind;
			mutable std::mutex PendingMutex;
			mutable std::unordered_map<DObject*, std::unique_ptr<FSceneBuildProduct>> Pending;
		};

		}

	auto MakeSceneImportRequest(
		const FSourcePath& MountedRootSource,
		const FAssetPath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		EImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FImportProvenance> ExistingProvenance,
		FImportRequest& OutRequest,
		std::string& OutError) -> bool
	{
		if (MountedRootSource.IsEmpty() || !DestinationDirectory.IsValid()
			|| !Settings.IsValid(&OutError))
		{
			if (OutError.empty()) OutError = "Scene AssetForge request is invalid.";
			return false;
		}
		const EImportOutputPolicy Policy = Mode == EImportMode::Import
			|| Mode == EImportMode::Preview
			? EImportOutputPolicy::Create : EImportOutputPolicy::ReplaceWholeState;
		if (Owner.OwnerId.empty()) Owner.OwnerId = "Scene.AssetForge";
		if (Owner.ConflictIdentities.empty())
			Owner.ConflictIdentities.push_back(DestinationDirectory.ToString());
		std::vector<FOutputMapping> ExistingMappings;
		if (ExistingProvenance) ExistingMappings = ExistingProvenance->OutputMappings;
		const FSchemaPayload Plan = EncodeSceneImportPlan({
			.DestinationDirectory = DestinationDirectory,
			.MeshSettings = Settings, .DefaultPolicy = Policy,
			.ExistingMappings = std::move(ExistingMappings)});
		std::optional<FImportProvenance> PersistedProvenance = std::move(ExistingProvenance);
		std::string ProvenanceError;
		if (PersistedProvenance && !PersistedProvenance->Validate(ProvenanceError))
			PersistedProvenance.reset();
		OutRequest = {
			.Mode = Mode, .RootSource = MountedRootSource,
			.TranslatorId = std::string(SceneTranslatorId), .TranslatorSettings = Plan,
			.PlanningPassStack = {{.PlanningPassId = std::string(ScenePlanningPassId),
				.ContractVersion = 1, .Settings = Plan}},
			.Destination = DestinationDirectory, .Owner = std::move(Owner),
			.ExistingProvenance = std::move(PersistedProvenance)};
		OutError.clear();
		return true;
	}

	auto MakeSceneRecordImportRequest(
		const DImportRecord& Record,
		EImportRecordAction,
		FImportOperationOwner Owner,
		FImportRequest& OutRequest,
		std::string& OutError) -> bool
	{
		const auto Root = std::ranges::find(
			Record.GetSources(), std::string_view("root"),
			&FImportRecordSource::StableIdentity);
		if (Root == Record.GetSources().end())
		{
			OutError = "Scene import record has no root source.";
			return false;
		}

		FAssetPath DestinationDirectory;
		FStaticMeshImportSettings MeshSettings;
		if (Record.GetProviderId() == SceneTranslatorId)
		{
			FSceneImportPlan PersistedPlan;
			const FSchemaPayload Settings = MakeSchemaPayload(
				Record.GetSettings().SchemaId, Record.GetSettings().SchemaVersion,
				std::vector<std::byte>(Record.GetSettings().Bytes));
			if (!DecodeSceneImportPlan(Settings, PersistedPlan, OutError)) return false;
			DestinationDirectory = PersistedPlan.DestinationDirectory;
			MeshSettings = PersistedPlan.MeshSettings;
		}
		else
		{
			OutError = std::format("Scene import record provider '{}'/{} is unsupported.",
				Record.GetProviderId(), Record.GetProviderContractVersion());
			return false;
		}

		FImportProvenance MappingCarrier;
		for (const FImportRecordOutput& Output : Record.GetOutputs())
			MappingCarrier.OutputMappings.push_back({
				.SourceNodeIdentity = Output.StableIdentity,
				.OutputIdentity = Output.StableIdentity,
				.AssetPath = Output.AssetPath});
		const std::string RecordPath = Record.GetPackage()
			? Record.GetPackage()->GetPackagePath() : std::string{};
		FAssetPath ParsedRecordPath;
		if (!FAssetPath::TryCreate(RecordPath, ParsedRecordPath, &OutError)) return false;
		MappingCarrier.OutputMappings.push_back({
			.SourceNodeIdentity = "scene-import-record",
			.OutputIdentity = "scene-import-record", .AssetPath = ParsedRecordPath});
		if (Owner.OwnerId.empty())
			Owner.OwnerId = std::format("Scene.RecordReimport:{}", RecordPath);
		if (Owner.ConflictIdentities.empty()) Owner.ConflictIdentities.push_back(RecordPath);
		return MakeSceneImportRequest(Root->SourcePath, DestinationDirectory,
			MeshSettings, EImportMode::Reimport, std::move(Owner),
			std::move(MappingCarrier), OutRequest, OutError);
	}

	auto RegisterSceneImportProvider(FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& Registrations,
		std::string& OutError) -> bool
	{
		auto Add = [&](FComponentRegistration Value) {
			if (!Value) return false;
			Registrations.push_back(std::move(Value));
			return true;
		};
		if (!Add(Service.RegisterSourceTranslatorScoped({.Descriptor = {
			.Identity = {.Id = std::string(SceneTranslatorId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(ScenePlanSchema), .SchemaVersion = 1}},
			.Extensions = {".gltf", ".glb", ".fbx", ".obj", ".dae", ".3ds", ".ply", ".stl"},
			.Priority = 110, .TranslationThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FSceneSourceTranslator>()}, OwnerGate, OutError))) return false;
		if (!Add(Service.RegisterPlanningPassScoped({.Descriptor = {
			.Identity = {.Id = std::string(ScenePlanningPassId), .ContractVersion = 1,
				.Settings = {.SchemaId = std::string(ScenePlanSchema), .SchemaVersion = 1}},
			.Priority = 110, .ExecutionThread = EThreadCapability::WorkerSafe},
			.Implementation = std::make_shared<FDefaultScenePlanningPass>()}, OwnerGate, OutError))) return false;
		for (size_t Index = 0; Index < SceneBuilderIds.size(); ++Index)
		{
			const ESceneOutputKind Kind = Index == 0 ? ESceneOutputKind::StaticMesh
				: Index == 1 ? ESceneOutputKind::MaterialInstance
				: Index == 2 ? ESceneOutputKind::Skeleton
				: Index == 3 ? ESceneOutputKind::SkeletalMesh
				: Index == 4 ? ESceneOutputKind::AnimationClip
				: Index == 5 ? ESceneOutputKind::Texture2D : ESceneOutputKind::ImportRecord;
			if (!Add(Service.RegisterAssetBuilderScoped({.Descriptor = {
				.Identity = {.Id = std::string(SceneBuilderIds[Index]), .ContractVersion = 1,
					.Settings = {.SchemaId = std::string(ScenePlanSchema), .SchemaVersion = 1}},
				.OutputClassName = std::string(SceneOutputClassName(Kind)), .Priority = 110,
				.ProductBuildThread = EThreadCapability::WorkerSafe},
				.Implementation = std::make_shared<FSceneAssetBuilder>(Kind)}, OwnerGate, OutError))) return false;
		}
		return true;
	}

	auto ClearSceneImportProviderCaches() -> void
	{
		std::lock_guard Lock(GSceneImportCacheMutex);
		GSceneImportCache.clear();
		GSceneImportOutputCache.clear();
	}}
