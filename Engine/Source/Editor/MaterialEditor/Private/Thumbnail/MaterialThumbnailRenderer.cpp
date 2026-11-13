#include "Engine/StreamableManager.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "RenderResource.h"
#include "DObject/WeakObjectPtr.h"

#include "Asset/Asset.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Package.h"
#include "Engine/Actor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Math/Operations.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Texture/Texture2D.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		constexpr uint32 MaterialThumbnailGeneratorSchema = 6;
		constexpr uint32 MaterialThumbnailShaderContract = 3;
		constexpr float MaterialThumbnailSphereScale = 1.65f;

		class FMaterialThumbnailGenerationInput final
			: public IAssetThumbnailGenerationInput
		{
		public:
			explicit FMaterialThumbnailGenerationInput(FTopLevelAssetPath InAssetPath, std::string InAssetClassName)
				: AssetPath(std::move(InAssetPath)), AssetClassName(std::move(InAssetClassName))
			{
			}

			FTopLevelAssetPath AssetPath;
			std::string AssetClassName;
		};

		auto MakeFingerprint(const FPackageData& Data,
			FTopLevelAssetPath AssetPath = {})
			-> FAssetThumbnailPackageFingerprint
		{
			const auto Asset = std::ranges::find(Data.TopLevelAssets, AssetPath, &FAssetData::AssetPath);
			return {
				.AssetPath = std::move(AssetPath),
				.PackagePath = Data.PackagePath,
				.AssetClassName = Asset == Data.TopLevelAssets.end() ? std::string{} : Asset->AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

		auto GetTextureDependencies(DMaterialInterface& Material) -> std::vector<DTexture2D*>
		{
			std::vector<DTexture2D*> Result;
			for (const auto& Definition : Material.GetParameterDefinitions())
			{
				if (Definition.Type != EMaterialParameterType::Texture) continue;
				DTexture2D* Texture = nullptr;
				if (Material.GetTextureParameterValue(Definition.Name, Texture) && Texture
					&& !std::ranges::contains(Result, Texture)) Result.push_back(Texture);
			}
			return Result;
		}

		struct FMaterialThumbnailResource
		{
			FMaterialResource Snapshot;
			bool bReady;
		};

		auto GetMaterialResourceSnapshot(
			DMaterialInterface* Material) -> std::expected<FMaterialThumbnailResource, std::string>
		{
			if (Material == nullptr)
			{
				return std::unexpected("The material asset is unavailable.");
			}
			DMaterialInterface* Current = Material;
			DMaterial* BaseMaterial = nullptr;
			std::unordered_set<DMaterialInterface*> Visited;
			while (Current != nullptr && Visited.insert(Current).second)
			{
				if ((BaseMaterial = Cast<DMaterial>(Current)) != nullptr) break;
				Current = Current->GetParent();
			}
			if (BaseMaterial == nullptr)
			{
				return std::unexpected(Current == nullptr
					? "The material instance has no valid parent."
					: "The material instance parent chain contains a cycle.");
			}
			const FMaterialCompileStatus& CompileStatus =
				Material->GetMaterialCompileStatus();
			if (!CompileStatus.IsCurrent()
				|| Material->GetAcceptedCompiledProgram() == nullptr)
			{
				if (Material->IsCompiling() || CompileStatus.HasScheduledEdits())
					return FMaterialThumbnailResource{{}, false};
				const auto Diagnostics = Material->GetMaterialCompileDiagnostics();
				return std::unexpected(!Diagnostics.empty() && !Diagnostics.front().Message.empty()
					? Diagnostics.front().Message
					: "The material has no current compiled program.");
			}
			for (DTexture2D* Texture : GetTextureDependencies(*Material))
			{
				if (!Texture->IsAsyncCacheComplete()) return FMaterialThumbnailResource{{}, false};
				if (!Texture->HasPlatformData())
				{
					return std::unexpected("A referenced material texture is not built.");
				}
				if (Texture->HasPendingRenderResourceInitialization()) return FMaterialThumbnailResource{{}, false};
				const auto* Resource = Texture->GetResource();
				if (!Resource || !Resource->GetTextureRHI_GameThread())
				{
					return std::unexpected("A referenced material texture render resource is unavailable.");
				}
			}
			return FMaterialThumbnailResource{Material->GetMaterialResource(), true};
		}

		auto GetMaterialAssetRevision(
			DMaterialInterface* Material) -> std::expected<uint64, std::string>
		{
			const DPackage* Package = Material ? Material->GetPackage() : nullptr;
			if (Package == nullptr)
			{
				return std::unexpected("The material asset package is unavailable.");
			}
			return Package->GetEditRevision();
		}

		auto MakeMaterialPreviewView() -> FThumbnailPreviewView
		{
			const FThumbnailVisualContract Contract;
			const FVector3 Eye = Math::Normalize(FVector3(
				Contract.CameraDirectionX,
				Contract.CameraDirectionY,
				Contract.CameraDirectionZ)) * static_cast<double>(Contract.CameraDistance);
			const FVector3 Forward = Math::Normalize(-Eye);
			const FVector3 Right = Math::Normalize(
				Math::Cross(FVectorConstants::Up, Forward));
			const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));
			return {
				.CameraPosition = {Eye.x, Eye.y, Eye.z},
				.CameraForward = {Forward.x, Forward.y, Forward.z},
				.CameraRight = {Right.x, Right.y, Right.z},
				.CameraUp = {Up.x, Up.y, Up.z},
				.VerticalFieldOfViewDegrees = Contract.VerticalFieldOfViewDegrees,
				.NearClipDistance = Contract.NearClipDistance,
				.FarClipDistance = Contract.FarClipDistance,
				.ClearRed = 0.0f,
				.ClearGreen = 0.0f,
				.ClearBlue = 0.0f,
				.ClearAlpha = 0.0f};
		}

		class FMaterialThumbnailGenerationSession final
			: public IThumbnailRendererSession
		{
		public:
			FMaterialThumbnailGenerationSession(
				FTopLevelAssetPath InAssetPath,
				std::string InAssetClassName)
				: AssetPath(std::move(InAssetPath))
				, AssetClassName(std::move(InAssetClassName))
			{
			}

			~FMaterialThumbnailGenerationSession() override
			{
				ResetPreview();
			}

			auto Load() -> FThumbnailRendererSessionUpdate override
			{
				ResetPreview();
				MaterialLoad = StreamableManager.RequestAsyncLoad(AssetPath, {}, DMaterialInterface::StaticClass());
				return {.State = EThumbnailRendererSessionState::WaitingForResources};
			}

			auto FinishLoad() -> FThumbnailRendererSessionUpdate
			{
				std::string SphereError;
				const bool Result = !MaterialLoad->HasError();
				Material = Result ? Cast<DMaterialInterface>(MaterialLoad->GetLoadedAsset()) : nullptr;
				if (!Result || Material == nullptr
					|| Material->GetClass()->GetQualifiedName().ToString() != AssetClassName)
				{
					Material = nullptr;
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = MaterialLoad->GetError().Message.empty()
							? "The requested asset is not an exact material class."
							: MaterialLoad->GetError().Message};
				}
				if (auto* Instance = Cast<DMaterialInstance>(Material);
					Instance != nullptr && Instance->GetParent() == nullptr)
				{
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = "The material instance has no valid parent."};
				}
				if (!SphereLoad)
				{
					FSoftObjectPath SpherePath;
					if (!FSoftObjectPath::TryCreate(
						FThumbnailVisualContract::SphereAssetPath, SpherePath))
						return {.Diagnostic = "The thumbnail sphere path is invalid."};
					SphereLoad = StreamableManager.RequestAsyncLoad(SpherePath, {}, DStaticMesh::StaticClass());
				}
				if (!SphereLoad->IsComplete())
					return {.State = EThumbnailRendererSessionState::WaitingForResources};
				SphereError = (SphereLoad->GetError().Message);
				if (SphereLoad->HasError()
					|| (Sphere = Cast<DStaticMesh>(SphereLoad->GetLoadedAsset())) == nullptr)
				{
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = SphereError.empty()
							? "The rendered-thumbnail sphere mesh is unavailable."
							: std::move(SphereError)};
				}
				const auto Revision = GetMaterialAssetRevision(Material);
				if (!Revision)
				{
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = Revision.error()};
				}
				AssetRevision = *Revision;
				bAssetsLoaded = true;
				return {
					.State = EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PollResources() -> FThumbnailRendererSessionUpdate override
			{
				if (!MaterialLoad) return {.Diagnostic = "The thumbnail load was reset."};
				if (!MaterialLoad->IsComplete())
					return {.State = EThumbnailRendererSessionState::WaitingForResources};
				if (!bAssetsLoaded)
				{
					const auto Loaded = FinishLoad();
					if (!bAssetsLoaded) return Loaded;
				}
				// Let the first asset builds run before admitting the larger fixed shader profile.
				if (!bShadersRequested && Material->GetMaterialCompileStatus().IsCurrent()
					&& Material->GetAcceptedCompiledProgram() && GEngine && GEngine->GetRendererModule())
				{
					Shaders = GEngine->GetRendererModule()->PrepareSceneThumbnailShaders();
					bShadersRequested = true;
				}
				auto Resource = GetMaterialResourceSnapshot(Material);
				if (!Resource)
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = std::move(Resource.error())};
				if (!Resource->bReady)
					return {
						.State = EThumbnailRendererSessionState::WaitingForResources};
				if (Sphere == nullptr)
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = "The rendered-thumbnail sphere mesh is unavailable."};
				// PostLoad schedules mesh compilation; asset residency does not imply render data is ready.
				if (Sphere->IsCompiling())
					return {
						.State = EThumbnailRendererSessionState::WaitingForResources};
				FStaticMeshRenderResourceStatus SphereStatus =
					Sphere->GetRenderResourceStatus();
				if (SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
				{
					Sphere->RequestRenderDataAndResources();
					if (Sphere->GetRenderDataLoadError().Code != ECookedMeshLoadError::None)
						return {
							.State = EThumbnailRendererSessionState::Failed,
							.Diagnostic = "The thumbnail sphere render data could not be loaded."};
					if (!Sphere->GetRenderData())
						return {.State = EThumbnailRendererSessionState::WaitingForResources};
					SphereStatus = Sphere->GetRenderResourceStatus();
				}
				if (SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::InvalidData
					|| SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = "The rendered-thumbnail sphere render resource is unavailable."};
				const bool bSphereReady = SphereStatus.Readiness
					== EStaticMeshRenderResourceReadiness::Ready;
				auto ShaderReady = Shaders.Poll();
				if (!ShaderReady) return {.Diagnostic = ShaderReady.error()};
				return {
					.State = Resource->bReady && bSphereReady && *ShaderReady
						? EThumbnailRendererSessionState::ReadyToRender
						: EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PreparePreview(
				IThumbnailPreviewScene& PreviewScene) -> std::expected<void, std::string> override
			{
				ResetScenePreview();
				if (!Material) { return std::unexpected("The material is unavailable."); }
				auto Resource = GetMaterialResourceSnapshot(Material);
				if (!Resource) return std::unexpected(std::move(Resource.error()));
				if (!Resource->bReady || !Sphere) return std::unexpected("The material preview resources are not ready.");
				PreparedMaterialSnapshot = std::move(Resource->Snapshot);
				PreparedSphereResourceRevision = Sphere->GetRenderResourceStatus().Revision;
				DependencySnapshots.clear();
				for (DTexture2D* Texture : GetTextureDependencies(*Material))
				{
					if (!Texture->IsAsyncCacheComplete() || Texture->HasPendingRenderResourceInitialization())
						return std::unexpected("The material texture resource is not ready.");
					const auto* TextureResource = Texture->GetResource();
					auto Snapshot = TextureResource ? TextureResource->GetTextureRHI_GameThread() : FTextureRHIRef{};
					if (!Snapshot)
					{
						return std::unexpected("The material texture snapshot is not ready.");
					}
					DependencySnapshots.push_back({Texture, std::move(Snapshot)});
				}
				World = PreviewScene.GetWorld();
				if (World == nullptr || Sphere == nullptr)
				{
					return std::unexpected("The rendered-thumbnail material preview is unavailable.");
				}
				Actor = PreviewScene.GetReusableActor("MaterialThumbnailPreviewActor");
				bOwnsActor = Actor == nullptr;
				if (bOwnsActor) Actor = World->SpawnActor<AActor>("MaterialThumbnailPreviewActor");
				Component = Actor ? Actor->FindComponentByExactClass<DStaticMeshComponent>() : nullptr;
				if (Actor && !Component)
					Component = Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
						DStaticMeshComponent::StaticClass(), "MaterialPreview"));
				if (Sphere == nullptr || Component == nullptr || Material == nullptr)
				{
					ResetPreview();
					return std::unexpected("The rendered-thumbnail material preview is unavailable.");
				}
				Component->SetStaticMesh(Sphere);
				for (uint32 SlotIndex = 0; SlotIndex < Component->GetNumMaterials(); ++SlotIndex)
					Component->SetMaterial(SlotIndex, Material);
				FTransform Transform;
				Transform.Scale3D = FVector3(MaterialThumbnailSphereScale);
				Component->SetWorldTransform(Transform);
				if (auto ViewResult = PreviewScene.SetView(MakeMaterialPreviewView()); !ViewResult)
				{
					ResetPreview();
					return std::unexpected(std::move(ViewResult.error()));
				}
				return {};
			}

			auto ValidatePreparedInput() const -> std::expected<void, std::string> override
			{
				if (Component == nullptr || !AreDependencySnapshotsCurrent())
				{
					return std::unexpected("A material texture changed while its thumbnail was being generated.");
				}
				auto MaterialAssetRevision = GetMaterialAssetRevision(Material);
				if (!MaterialAssetRevision) return std::unexpected(std::move(MaterialAssetRevision.error()));
				auto Resource = GetMaterialResourceSnapshot(Material);
				if (!Resource) return std::unexpected(std::move(Resource.error()));
				const FStaticMeshRenderResourceStatus SphereStatus = Sphere
					? Sphere->GetRenderResourceStatus()
					: FStaticMeshRenderResourceStatus{};
				if (!Resource->bReady || Material == nullptr
					|| SphereStatus.Readiness != EStaticMeshRenderResourceReadiness::Ready
					|| *MaterialAssetRevision != AssetRevision
					|| Resource->Snapshot != PreparedMaterialSnapshot
					|| SphereStatus.Revision != PreparedSphereResourceRevision)
				{
					return std::unexpected("The material changed while its thumbnail was being generated.");
				}
				return {};
			}

			auto ResetPreview() -> void override
			{
				ResetScenePreview();
				DependencySnapshots.clear();
				PreparedMaterialSnapshot = {};
				Sphere = nullptr;
				Material = nullptr;
				bAssetsLoaded = false;
				if (MaterialLoad) MaterialLoad->CancelHandle();
				if (SphereLoad) SphereLoad->CancelHandle();
				MaterialLoad.reset();
				SphereLoad.reset();
				Shaders = {};
				bShadersRequested = false;
			}

		private:
			FShaderMapPreparation Shaders;
			bool bShadersRequested = false;
			auto AreDependencySnapshotsCurrent() const -> bool
			{
				return std::ranges::all_of(DependencySnapshots, [](const auto& Item) {
					const auto* Texture = Item.Texture.Get();
					return Texture && Texture->IsAsyncCacheComplete() && !Texture->HasPendingRenderResourceInitialization()
						&& Texture->GetResource()
						&& Texture->GetResource()->GetTextureRHI_GameThread() == Item.Allocation;
				});
			}
			// Fixed allocations stay alive independently of their weak asset identities.
			struct FDependencySnapshot
			{
				TWeakObjectPtr<DTexture2D> Texture;
				FTextureRHIRef Allocation;
			};
			std::vector<FDependencySnapshot> DependencySnapshots;
			auto ResetScenePreview() -> void
			{
				if (Component != nullptr && !bOwnsActor)
				{
					for (uint32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
						Component->SetMaterial(Slot, nullptr);
					Component->SetStaticMesh(nullptr);
				}
				if (World != nullptr && Actor != nullptr && bOwnsActor) World->DestroyActor(Actor);
				Component = nullptr;
				Actor = nullptr;
				World = nullptr;
			}

			FTopLevelAssetPath AssetPath;
			std::string AssetClassName;
			DMaterialInterface* Material = nullptr;
			uint64 AssetRevision = 0;
			uint64 PreparedSphereResourceRevision = 0;
			FMaterialResource PreparedMaterialSnapshot;
			DWorld* World = nullptr;
			AActor* Actor = nullptr;
			bool bOwnsActor = false;
			DStaticMeshComponent* Component = nullptr;
			DStaticMesh* Sphere = nullptr;
			FStreamableManager StreamableManager;
			std::shared_ptr<FStreamableHandle> MaterialLoad;
			std::shared_ptr<FStreamableHandle> SphereLoad;
			bool bAssetsLoaded = false;
		};
	} // namespace

	DMaterialThumbnailRenderer::DMaterialThumbnailRenderer(
		const FObjectInitializer& Initializer)
		: Super(Initializer)
	{
	}

	auto DMaterialThumbnailRenderer::GetRegistration() const
		-> FThumbnailRenderingInfo
	{
		return {
			.RendererName = "MaterialRenderedThumbnail",
			.GeneratorSchemaVersion = MaterialThumbnailGeneratorSchema};
	}

	auto DMaterialThumbnailRenderer::CaptureGenerationRequest(
		const FAssetThumbnailRequest& Request,
		uint64 RendererGeneration) -> std::expected<FAssetThumbnailGenerationRequest, std::string>
	{
		FAssetThumbnailGenerationRequest GenerationRequest;

		if (Request.Asset.AssetClassName != DMaterial::StaticClass()->GetQualifiedName().ToString()
			&& Request.Asset.AssetClassName != DMaterialInstance::StaticClass()->GetQualifiedName().ToString())
		{
			return std::unexpected("The material thumbnail renderer received the wrong asset class.");
		}

		const FAssetDependencyClosureSnapshot Closure =
			CaptureAssetDependencyClosure(Request.Asset.PackagePath);
		if (!Closure)
		{
			return std::unexpected(FormatAssetRegistryError(Closure.Result).empty()
				? std::format("Material thumbnail registry data is missing for {}.",
					Request.Asset.AssetPath.ToString())
				: FormatAssetRegistryError(Closure.Result));
		}
		const auto RootIt = std::ranges::find_if(
			Closure.Assets,
			[&Request](const FPackageData& Data) {
				return Data.PackagePath == Request.Asset.PackagePath;
			});
		if (RootIt == Closure.Assets.end())
		{
			return std::unexpected("The material dependency closure omitted its root asset.");
		}
		const FPackageData* Root = &*RootIt;
		if (MakeFingerprint(*Root, Request.Asset.AssetPath) != Request.Asset)
		{
			return std::unexpected(std::format(
				"Material thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.AssetPath.ToString()));
		}
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
		Dependencies.reserve(Closure.Assets.size() - 1);
		for (const FPackageData& Data : Closure.Assets)
			if (Data.PackagePath != Request.Asset.PackagePath)
				Dependencies.push_back(MakeFingerprint(Data));

		const FThumbnailVisualContract Visual;
		GenerationRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				FThumbnailVisualContract::SphereAssetPath),
			.PreviewFixtureVersion =
				FThumbnailVisualContract::SphereFixtureVersion,
			.ShaderContractVersion = MaterialThumbnailShaderContract,
			.Dependencies = std::move(Dependencies)};
		GenerationRequest.Input =
			std::make_shared<FMaterialThumbnailGenerationInput>(Request.Asset.AssetPath, Request.Asset.AssetClassName);
		GenerationRequest.RendererGeneration = RendererGeneration;
		GenerationRequest.RequestSerial = Request.RequestSerial;
		return GenerationRequest;
	}

	auto DMaterialThumbnailRenderer::CreateGenerationSession(
		const FAssetThumbnailGenerationRequest&,
		const IAssetThumbnailGenerationInput& Input)
		-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string>
	{
		const auto* MaterialInput = dynamic_cast<const FMaterialThumbnailGenerationInput*>(&Input);
		if (MaterialInput == nullptr)
		{
			return std::unexpected("The material thumbnail generation input is invalid.");
		}
		return std::make_unique<FMaterialThumbnailGenerationSession>(
			MaterialInput->AssetPath, MaterialInput->AssetClassName);
	}

} // namespace Durin::MaterialEditor
