#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
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

		class FMaterialThumbnailDrawState final
		{
		public:
			FMaterialThumbnailDrawState(DThumbnailRenderer& InRenderer, DObject* Object, FTopLevelAssetPath Path)
				: Renderer(InRenderer), AssetPath(std::move(Path))
			{
				Material = Cast<DMaterialInterface>(Object);
			}

			~FMaterialThumbnailDrawState()
			{
				ResetPreview();
			}

			auto FinishLoad(FThumbnailDrawContext& Context) -> FThumbnailDrawResult
			{
				if (auto* Instance = Cast<DMaterialInstance>(Material); Instance && !Instance->GetParent())
					return {.Diagnostic = "The material instance has no valid parent."};
				FSoftObjectPath Path;
				if (!FSoftObjectPath::TryCreate(FThumbnailVisualContract::SphereAssetPath, Path))
					return {.Diagnostic = "The thumbnail sphere path is invalid."};
				Sphere = Cast<DStaticMesh>(Context.LoadAsset(Path, DStaticMesh::StaticClass()));
				if (!Sphere) return Context.GetResult();
				const auto Revision = GetMaterialAssetRevision(Material);
				if (!Revision) return {.Diagnostic = Revision.error()};
				AssetRevision = *Revision; bAssetsLoaded = true;
				return {.State = EThumbnailDrawStatus::WaitingForResources};
			}

			auto Update(FThumbnailDrawContext& Context) -> FThumbnailDrawResult
			{
				if (!bAssetsLoaded)
				{
					const auto Loaded = FinishLoad(Context);
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
						.State = EThumbnailDrawStatus::Failed,
						.Diagnostic = std::move(Resource.error())};
				if (!Resource->bReady)
					return {
						.State = EThumbnailDrawStatus::WaitingForResources};
				if (Sphere == nullptr)
					return {
						.State = EThumbnailDrawStatus::Failed,
						.Diagnostic = "The rendered-thumbnail sphere mesh is unavailable."};
				// PostLoad schedules mesh compilation; asset residency does not imply render data is ready.
				if (Sphere->IsCompiling())
					return {
						.State = EThumbnailDrawStatus::WaitingForResources};
				FStaticMeshRenderResourceStatus SphereStatus =
					Sphere->GetRenderResourceStatus();
				if (SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
				{
					Sphere->RequestRenderDataAndResources();
					if (Sphere->GetRenderDataLoadError().Code != ECookedMeshLoadError::None)
						return {
							.State = EThumbnailDrawStatus::Failed,
							.Diagnostic = "The thumbnail sphere render data could not be loaded."};
					if (!Sphere->GetRenderData())
						return {.State = EThumbnailDrawStatus::WaitingForResources};
					SphereStatus = Sphere->GetRenderResourceStatus();
				}
				if (SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::InvalidData
					|| SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
					return {
						.State = EThumbnailDrawStatus::Failed,
						.Diagnostic = "The rendered-thumbnail sphere render resource is unavailable."};
				const bool bSphereReady = SphereStatus.Readiness
					== EStaticMeshRenderResourceReadiness::Ready;
				auto ShaderReady = Shaders.Poll();
				if (!ShaderReady) return {.Diagnostic = ShaderReady.error()};
				return {
					.State = Resource->bReady && bSphereReady && *ShaderReady
						? EThumbnailDrawStatus::ReadyToRender
						: EThumbnailDrawStatus::WaitingForResources};
			}

			auto PreparePreview(
				FThumbnailDrawContext& Context) -> std::expected<void, std::string>
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
				auto& Scene = Renderer.GetPreviewScene();
				if (auto Bound = Context.SetScene(Scene); !Bound) return Bound;
				AActor* Actor = Scene.GetPreviewActor();
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
				Context.SetView(MakeMaterialPreviewView());
				return {};
			}

			auto ValidatePreparedInput() const -> std::expected<void, std::string>
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

			auto ResetPreview() -> void
			{
				ResetScenePreview();
				DependencySnapshots.clear();
				PreparedMaterialSnapshot = {};
				Sphere = nullptr;
				Material = nullptr;
				bAssetsLoaded = false;
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
				if (Component != nullptr)
				{
					for (uint32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
						Component->SetMaterial(Slot, nullptr);
					Component->SetStaticMesh(nullptr);
				}
				Component = nullptr;
			}

			FTopLevelAssetPath AssetPath;
			DMaterialInterface* Material = nullptr;
			uint64 AssetRevision = 0;
			uint64 PreparedSphereResourceRevision = 0;
			FMaterialResource PreparedMaterialSnapshot;
			DThumbnailRenderer& Renderer;
			DStaticMeshComponent* Component = nullptr;
			DStaticMesh* Sphere = nullptr;
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

	auto DMaterialThumbnailRenderer::DescribeThumbnail(const FAssetThumbnailPackageFingerprint& Asset) -> std::expected<FThumbnailCacheDescription, std::string>
	{
		FThumbnailCacheDescription Result;

		if (Asset.AssetClassName != DMaterial::StaticClass()->GetQualifiedName().ToString()
			&& Asset.AssetClassName != DMaterialInstance::StaticClass()->GetQualifiedName().ToString())
		{
			return std::unexpected("The material thumbnail renderer received the wrong asset class.");
		}

		const FAssetDependencyClosureSnapshot Closure =
			CaptureAssetDependencyClosure(Asset.PackagePath);
		if (!Closure)
		{
			return std::unexpected(FormatAssetRegistryError(Closure.Result).empty()
				? std::format("Material thumbnail registry data is missing for {}.",
					Asset.AssetPath.ToString())
				: FormatAssetRegistryError(Closure.Result));
		}
		const auto RootIt = std::ranges::find_if(
			Closure.Assets,
			[&Asset](const FPackageData& Data) {
				return Data.PackagePath == Asset.PackagePath;
			});
		if (RootIt == Closure.Assets.end())
		{
			return std::unexpected("The material dependency closure omitted its root asset.");
		}
		const FPackageData* Root = &*RootIt;
		if (MakeFingerprint(*Root, Asset.AssetPath) != Asset)
		{
			return std::unexpected(std::format(
				"Material thumbnail registry data changed for {}; refresh the request snapshot.",
				Asset.AssetPath.ToString()));
		}
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
		Dependencies.reserve(Closure.Assets.size() - 1);
		for (const FPackageData& Data : Closure.Assets)
			if (Data.PackagePath != Asset.PackagePath)
				Dependencies.push_back(MakeFingerprint(Data));

		const FThumbnailVisualContract Visual;
		Result = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				FThumbnailVisualContract::SphereAssetPath),
			.PreviewFixtureVersion =
				FThumbnailVisualContract::SphereFixtureVersion,
			.ShaderContractVersion = MaterialThumbnailShaderContract,
			.Dependencies = std::move(Dependencies)};

		return Result;
	}

	auto DMaterialThumbnailRenderer::CanVisualizeAsset(DObject* Object) const -> bool
	{
		return Cast<DMaterialInterface>(Object) != nullptr;
	}

	auto DMaterialThumbnailRenderer::Draw(DObject* Object, FThumbnailDrawContext& Context) -> void
	{
		if (!CanVisualizeAsset(Object)) { Context.SetResult({.Diagnostic = "The renderer cannot visualize this asset."}); return; }
		auto& State = Context.GetState<FMaterialThumbnailDrawState>(*this, Object, Context.GetAssetPath());
		auto Result = State.Update(Context);
		Context.SetResult(Result);
		if (Result.State == EThumbnailDrawStatus::ReadyToRender)
		{
			if (auto Prepared = State.PreparePreview(Context); !Prepared)
			{
				if (Context.GetResult().State == EThumbnailDrawStatus::ReadyToRender)
					Context.SetResult({.Diagnostic = Prepared.error()});
			}
			else Context.SetValidation([&State] { return State.ValidatePreparedInput(); });
		}
	}

} // namespace Durin::MaterialEditor
