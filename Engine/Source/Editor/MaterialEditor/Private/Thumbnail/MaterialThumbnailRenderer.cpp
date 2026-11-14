#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "RenderResource.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/StreamableManager.h"

#include "Asset/Asset.h"
#include "Components/StaticMeshComponent.h"
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
		constexpr float MaterialThumbnailSphereScale = 1.65f;

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
			std::shared_ptr<const FMaterialProgram> Program;
			uint64 RenderRevision = 0;
			FMaterialStaticProperties Properties;
			bool bReady = false;
			auto operator==(const FMaterialThumbnailResource&) const -> bool = default;
		};

		auto QueryMaterialThumbnailReadiness(
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
					return FMaterialThumbnailResource{};
				const auto Diagnostics = Material->GetMaterialCompileDiagnostics();
				return std::unexpected(!Diagnostics.empty() && !Diagnostics.front().Message.empty()
					? Diagnostics.front().Message
					: "The material has no current compiled program.");
			}
			for (DTexture2D* Texture : GetTextureDependencies(*Material))
			{
				if (!Texture->IsAsyncCacheComplete()) return FMaterialThumbnailResource{};
				if (!Texture->HasPlatformData())
				{
					return std::unexpected("A referenced material texture is not built.");
				}
				if (Texture->HasPendingRenderResourceInitialization()) return FMaterialThumbnailResource{};
				const auto* Resource = Texture->GetResource();
				if (!Resource || !Resource->GetTextureRHI_GameThread())
				{
					return std::unexpected("A referenced material texture render resource is unavailable.");
				}
			}
			return FMaterialThumbnailResource{Material->GetAcceptedCompiledProgram(), Material->GetRenderChangeCount(), Material->GetMaterialResourceProperties(), true};
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

	} // namespace

	DMaterialThumbnailRenderer::DMaterialThumbnailRenderer(
		const FObjectInitializer& Initializer)
		: Super(Initializer)
	{
	}

	auto DMaterialThumbnailRenderer::BeginDestroy() -> void
	{
		if (SphereLoad) SphereLoad->CancelHandle();
		SphereLoad.reset();
		Shaders.reset();
		Super::BeginDestroy();
	}

	auto DMaterialThumbnailRenderer::CanVisualizeAsset(DObject* Object) const -> bool
	{
		return Cast<DMaterialInterface>(Object) != nullptr;
	}

	auto DMaterialThumbnailRenderer::Draw(DObject* Object, FThumbnailDrawContext& Context) -> void
	{
		auto* Material = Cast<DMaterialInterface>(Object);
		if (!Material) return Context.Fail("The renderer cannot visualize this asset.");
		if (auto* Instance = Cast<DMaterialInstance>(Material); Instance && !Instance->GetParent())
			return Context.Fail("The material instance has no valid parent.");
		if (!SphereLoad)
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(FThumbnailVisualContract::SphereAssetPath, Path))
				return Context.Fail("The thumbnail sphere path is invalid.");
			SphereLoad = GetStreamableManager().RequestAsyncLoad(Path, {}, DStaticMesh::StaticClass());
		}
		if (!SphereLoad) return Context.Fail("The thumbnail sphere load is unavailable.");
		if (!SphereLoad->IsComplete()) return;
		if (SphereLoad->HasError())
		{
			auto Error = SphereLoad->GetError().Message;
			SphereLoad.reset();
			return Context.Fail(std::move(Error));
		}
		auto* Sphere = Cast<DStaticMesh>(SphereLoad->GetLoadedAsset());
		if (!Sphere) return Context.Fail("The thumbnail sphere mesh is unavailable.");
		if (!Shaders && Material->GetMaterialCompileStatus().IsCurrent()
			&& Material->GetAcceptedCompiledProgram() && GEngine && GEngine->GetRendererModule())
			Shaders = GEngine->GetRendererModule()->PrepareSceneThumbnailShaders();
		auto Resource = QueryMaterialThumbnailReadiness(Material);
		if (!Resource) return Context.Fail(Resource.error());
		if (!Resource->bReady || !Shaders) return;
		// PostLoad schedules mesh compilation; asset residency does not imply render data is ready.
		if (Sphere->IsCompiling())
			return;
		FStaticMeshRenderResourceStatus SphereStatus =
			Sphere->GetRenderResourceStatus();
		if (SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
		{
			Sphere->RequestRenderDataAndResources();
			if (Sphere->GetRenderDataLoadError().Code != ECookedMeshLoadError::None)
				return Context.Fail("The thumbnail sphere render data could not be loaded.");
			if (!Sphere->GetRenderData())
				return;
			SphereStatus = Sphere->GetRenderResourceStatus();
		}
		if (SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::InvalidData
			|| SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
			return Context.Fail("The rendered-thumbnail sphere render resource is unavailable.");
		const bool bSphereReady = SphereStatus.Readiness
			== EStaticMeshRenderResourceReadiness::Ready;
		auto ShaderReady = Shaders->Poll();
		if (!ShaderReady) return Context.Fail(ShaderReady.error());
		if (!bSphereReady || !*ShaderReady) return;
		const auto PreparedMaterialSnapshot = *Resource;
		const auto PreparedSphereResourceRevision = Sphere->GetRenderResourceStatus().Revision;
		struct FDependencySnapshot { TWeakObjectPtr<DTexture2D> Texture; FTextureRHIRef Allocation; };
		std::vector<FDependencySnapshot> DependencySnapshots;
		for (DTexture2D* Texture : GetTextureDependencies(*Material))
		{
			if (!Texture->IsAsyncCacheComplete() || Texture->HasPendingRenderResourceInitialization())
				return Context.Fail("The material texture resource is not ready.");
			const auto* TextureResource = Texture->GetResource();
			auto Snapshot = TextureResource ? TextureResource->GetTextureRHI_GameThread() : FTextureRHIRef{};
			if (!Snapshot)
			{
				return Context.Fail("The material texture snapshot is not ready.");
			}
			DependencySnapshots.push_back({Texture, std::move(Snapshot)});
		}
		auto& Scene = GetPreviewScene();
		if (!Context.TrySetScene(Scene)) return;
		AActor* Actor = Scene.GetPreviewActor();
		auto* Component = Actor ? Actor->FindComponentByExactClass<DStaticMeshComponent>() : nullptr;
		if (Actor && !Component)
			Component = Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
				DStaticMeshComponent::StaticClass(), "MaterialPreview"));
		if (Component == nullptr)
		{
			return Context.Fail("The rendered-thumbnail material preview is unavailable.");
		}
		Component->SetStaticMesh(Sphere);
		for (uint32 SlotIndex = 0; SlotIndex < Component->GetNumMaterials(); ++SlotIndex)
			Component->SetMaterial(SlotIndex, Material);
		FTransform Transform;
		Transform.Scale3D = FVector3(MaterialThumbnailSphereScale);
		Component->SetWorldTransform(Transform);
		Context.SetView(MakeMaterialPreviewView());
		Context.SetValidation([Material = TWeakObjectPtr<DMaterialInterface>(Material),
			Sphere = TWeakObjectPtr<DStaticMesh>(Sphere), PreparedMaterialSnapshot,
			PreparedSphereResourceRevision, Dependencies = std::move(DependencySnapshots)] {
			if (!Material.IsValid() || !Sphere.IsValid()) return false;
			auto Current = QueryMaterialThumbnailReadiness(Material.Get());
			const auto MeshStatus = Sphere.Get()->GetRenderResourceStatus();
			return Current && Current->bReady && *Current == PreparedMaterialSnapshot
				&& MeshStatus.Readiness == EStaticMeshRenderResourceReadiness::Ready
				&& MeshStatus.Revision == PreparedSphereResourceRevision
				&& std::ranges::all_of(Dependencies, [](const auto& Item) {
					const auto* Texture = Item.Texture.Get();
					return Texture && Texture->IsAsyncCacheComplete() && !Texture->HasPendingRenderResourceInitialization()
						&& Texture->GetResource() && Texture->GetResource()->GetTextureRHI_GameThread() == Item.Allocation;
				});
		});
		Context.MarkReady();
	}

} // namespace Durin::MaterialEditor
