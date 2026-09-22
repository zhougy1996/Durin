#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "DObject/WeakObjectPtr.h"

#include "Asset/Asset.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Package.h"
#include "Engine/Actor.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Math/Operations.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Texture/Texture2D.h"

#include <unordered_set>

namespace Durin::Editor::Material
{
	namespace
	{
		constexpr uint32 MaterialThumbnailGeneratorSchema = 6;
		constexpr uint32 MaterialThumbnailShaderContract = 3;
		constexpr float MaterialThumbnailSphereScale = 1.65f;

		class FMaterialThumbnailGenerationInput final
			: public ::Durin::Editor::IAssetThumbnailGenerationInput
		{
		public:
			explicit FMaterialThumbnailGenerationInput(FTopLevelAssetPath InAssetPath)
				: AssetPath(std::move(InAssetPath))
			{
			}

			FTopLevelAssetPath AssetPath;
		};

		auto MakeFingerprint(const FAssetData& Data,
			FTopLevelAssetPath AssetPath = {})
			-> ::Durin::Editor::FAssetThumbnailPackageFingerprint
		{
			return {
				.AssetPath = std::move(AssetPath),
				.PackagePath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
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
			uint64 Revision;
			bool bReady;
		};

		auto GetMaterialResourceRevision(
			DMaterialInterface* Material) -> std::expected<FMaterialThumbnailResource, std::string>
		{
			if (Material == nullptr)
			{
				return std::unexpected("The material asset is unavailable.");
			}
			uint64 Revision = 0;
			DMaterialInterface* Current = Material;
			DMaterial* BaseMaterial = nullptr;
			std::unordered_set<DMaterialInterface*> Visited;
			while (Current != nullptr && Visited.insert(Current).second)
			{
				Revision ^= Current->GetRenderStateVersion() + 0x9e3779b97f4a7c15ull
					+ (Revision << 6) + (Revision >> 2);
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
				if (CompileStatus.State == EMaterialCompileState::Deferred
					|| CompileStatus.State == EMaterialCompileState::Pending
					|| CompileStatus.State == EMaterialCompileState::Running)
					return FMaterialThumbnailResource{Revision == 0 ? 1 : Revision, false};
				const auto Diagnostics = Material->GetMaterialCompileDiagnostics();
				return std::unexpected(!Diagnostics.empty() && !Durin::FormatMaterialError(Diagnostics.front().Source.Error).empty()
					? Durin::FormatMaterialError(Diagnostics.front().Source.Error)
					: "The material has no current compiled program.");
			}
			for (DTexture2D* Texture : GetTextureDependencies(*Material))
			{
				if (!Texture->IsAsyncCacheComplete()) return FMaterialThumbnailResource{Revision, false};
				if (!Texture->HasPlatformData())
				{
					return std::unexpected("A referenced material texture is not built.");
				}
				if (Texture->IsResourceUpdatePending()) return FMaterialThumbnailResource{Revision, false};
				if (Texture->GetResourceUpdateState() == ETextureResourceUpdateState::Failed
					&& !Texture->HasUsableResource())
				{
					return std::unexpected("A referenced material texture render resource failed.");
				}
				if (!Texture->HasUsableResource()) return FMaterialThumbnailResource{Revision, false};
			}
			return FMaterialThumbnailResource{Revision == 0 ? 1 : Revision, true};
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

		auto CombineResourceRevision(uint64 MaterialRevision, uint64 MeshRevision)
			-> uint64
		{
			uint64 Revision = MaterialRevision;
			Revision ^= MeshRevision + 0x9e3779b97f4a7c15ull
				+ (Revision << 6) + (Revision >> 2);
			return Revision == 0 ? 1 : Revision;
		}

		auto MakeMaterialPreviewView() -> ::Durin::Editor::FThumbnailPreviewView
		{
			const ::Durin::Editor::FThumbnailVisualContract Contract;
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
			: public ::Durin::Editor::IThumbnailRendererSession
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

			auto Load() -> ::Durin::Editor::FThumbnailRendererSessionUpdate override
			{
				ResetPreview();
				MaterialLoad = RequestAsyncLoad(AssetPath, {}, DMaterialInterface::StaticClass());
				return {.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
			}

			auto FinishLoad() -> ::Durin::Editor::FThumbnailRendererSessionUpdate
			{
				std::string SphereError;
				const auto& Result = MaterialLoad->GetResult();
				Material = Result ? Cast<DMaterialInterface>(MaterialLoad->GetLoadedObject()) : nullptr;
				if (!Result || Material == nullptr
					|| Material->GetClass()->GetQualifiedName().ToString() != AssetClassName)
				{
					Material = nullptr;
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = (Result ? std::string{} : Result.error().Message).empty()
							? "The requested asset is not an exact material class."
							: (Result ? std::string{} : Result.error().Message)};
				}
				if (auto* Instance = Cast<DMaterialInstance>(Material);
					Instance != nullptr && Instance->GetParent() == nullptr)
				{
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = "The material instance has no valid parent."};
				}
				if (!SphereLoad)
				{
					FObjectPath SpherePath;
					if (!FObjectPath::TryCreate(
						::Durin::Editor::FThumbnailVisualContract::SphereAssetPath, SpherePath))
						return {.Diagnostic = "The thumbnail sphere path is invalid."};
					SphereLoad = RequestAsyncLoad(SpherePath, {}, DStaticMesh::StaticClass());
				}
				if (!SphereLoad->IsComplete())
					return {.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
				SphereError = (SphereLoad->GetResult() ? std::string{} : SphereLoad->GetResult().error().Message);
				if (!SphereLoad->GetResult()
					|| (Sphere = Cast<DStaticMesh>(SphereLoad->GetLoadedObject())) == nullptr)
				{
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = SphereError.empty()
							? "The rendered-thumbnail sphere mesh is unavailable."
							: std::move(SphereError)};
				}
				const auto Revision = GetMaterialAssetRevision(Material);
				if (!Revision)
				{
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = Revision.error()};
				}
				AssetRevision = *Revision;
				bAssetsLoaded = true;
				return {
					.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PollResources() -> ::Durin::Editor::FThumbnailRendererSessionUpdate override
			{
				if (!MaterialLoad) return {.Diagnostic = "The thumbnail load was reset."};
				if (!MaterialLoad->IsComplete())
					return {.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
				if (!bAssetsLoaded)
				{
					const auto Loaded = FinishLoad();
					if (!bAssetsLoaded) return Loaded;
				}
				auto Resource = GetMaterialResourceRevision(Material);
				if (!Resource)
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = std::move(Resource.error())};
				if (!Resource->bReady)
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
				if (Sphere == nullptr)
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = "The rendered-thumbnail sphere mesh is unavailable."};
				// PostLoad schedules mesh compilation; asset residency does not imply render data is ready.
				if (HasPendingStaticMeshCompilation(*Sphere))
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
				FStaticMeshRenderResourceStatus SphereStatus =
					Sphere->GetRenderResourceStatus();
				if (SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
				{
					const auto LoadStatus = Sphere->RequestRenderDataAndResources();
					if (LoadStatus.CpuPhase == ECookedMeshCpuPhase::Failed
						|| LoadStatus.CpuPhase == ECookedMeshCpuPhase::Cancelled)
						return {
							.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
							.Diagnostic = "The thumbnail sphere render data could not be loaded."};
					if (!LoadStatus.HasCpuData())
						return {.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
					SphereStatus = Sphere->GetRenderResourceStatus();
				}
				if (SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Failed
					|| SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = "The rendered-thumbnail sphere render resource is unavailable."};
				const bool bSphereReady = SphereStatus.Readiness
					== EStaticMeshRenderResourceReadiness::Ready;
				return {
					.State = Resource->bReady && bSphereReady
						? ::Durin::Editor::EThumbnailRendererSessionState::ReadyToRender
						: ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PreparePreview(
				::Durin::Editor::IThumbnailPreviewScene& PreviewScene) -> std::expected<void, std::string> override
			{
				ResetScenePreview();
				if (!Material) { return std::unexpected("The material is unavailable."); }
				auto Resource = GetMaterialResourceRevision(Material);
				if (!Resource) return std::unexpected(std::move(Resource.error()));
				if (!Resource->bReady || !Sphere) return std::unexpected("The material preview resources are not ready.");
				PreparedResourceRevision = CombineResourceRevision(Resource->Revision, Sphere->GetRenderResourceStatus().Revision);
				DependencySnapshots.clear();
				for (DTexture2D* Texture : GetTextureDependencies(*Material))
				{
					auto Snapshot = Texture->GetPublishedTexture();
					if (!Snapshot || Texture->IsResourceUpdatePending())
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
				Actor = World->SpawnActor<AActor>("MaterialThumbnailPreviewActor");
				Component = Actor
					? Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
						DStaticMeshComponent::StaticClass(), "MaterialPreview"))
					: nullptr;
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
				auto Resource = GetMaterialResourceRevision(Material);
				if (!Resource) return std::unexpected(std::move(Resource.error()));
				const FStaticMeshRenderResourceStatus SphereStatus = Sphere
					? Sphere->GetRenderResourceStatus()
					: FStaticMeshRenderResourceStatus{};
				const uint64 Revision = CombineResourceRevision(
					Resource->Revision, SphereStatus.Revision);
				if (!Resource->bReady || Material == nullptr
					|| SphereStatus.Readiness != EStaticMeshRenderResourceReadiness::Ready
					|| *MaterialAssetRevision != AssetRevision
					|| Revision != PreparedResourceRevision)
				{
					return std::unexpected("The material changed while its thumbnail was being generated.");
				}
				return {};
			}

			auto ResetPreview() -> void override
			{
				ResetScenePreview();
				DependencySnapshots.clear();
				Sphere = nullptr;
				Material = nullptr;
				bAssetsLoaded = false;
				if (MaterialLoad) MaterialLoad->Cancel();
				if (SphereLoad) SphereLoad->Cancel();
				MaterialLoad.reset();
				SphereLoad.reset();
			}

		private:
			auto AreDependencySnapshotsCurrent() const -> bool
			{
				return std::ranges::all_of(DependencySnapshots, [](const auto& Item) {
					const auto* Texture = Item.Texture.Get();
					return Texture && !Texture->IsResourceUpdatePending()
						&& Texture->GetPublishedTexture() == Item.Allocation;
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
				if (World != nullptr && Actor != nullptr) World->DestroyActor(Actor);
				Component = nullptr;
				Actor = nullptr;
				World = nullptr;
			}

			FTopLevelAssetPath AssetPath;
			std::string AssetClassName;
			DMaterialInterface* Material = nullptr;
			uint64 AssetRevision = 0;
			uint64 PreparedResourceRevision = 0;
			DWorld* World = nullptr;
			AActor* Actor = nullptr;
			DStaticMeshComponent* Component = nullptr;
			DStaticMesh* Sphere = nullptr;
			std::shared_ptr<FAsyncLoadHandle> MaterialLoad;
			std::shared_ptr<FAsyncLoadHandle> SphereLoad;
			bool bAssetsLoaded = false;
		};
	} // namespace

	DMaterialThumbnailRenderer::DMaterialThumbnailRenderer(
		std::string InAssetClassName)
		: AssetClassName(std::move(InAssetClassName))
	{
	}

	auto DMaterialThumbnailRenderer::GetRegistration() const
		-> ::Durin::Editor::FThumbnailRenderingInfo
	{
		return {
			.AssetClassName = AssetClassName,
			.RendererName = "MaterialRenderedThumbnail",
			.GeneratorSchemaVersion = MaterialThumbnailGeneratorSchema};
	}

	auto DMaterialThumbnailRenderer::CaptureGenerationRequest(
		const ::Durin::Editor::FAssetThumbnailRequest& Request,
		uint64 RendererGeneration) -> std::expected<::Durin::Editor::FAssetThumbnailGenerationRequest, std::string>
	{
		::Durin::Editor::FAssetThumbnailGenerationRequest GenerationRequest;

		if (Request.Asset.AssetClassName != AssetClassName)
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
			[&Request](const FAssetData& Data) {
				return Data.PackagePath == Request.Asset.PackagePath;
			});
		if (RootIt == Closure.Assets.end())
		{
			return std::unexpected("The material dependency closure omitted its root asset.");
		}
		const FAssetData* Root = &*RootIt;
		if (MakeFingerprint(*Root, Request.Asset.AssetPath) != Request.Asset)
		{
			return std::unexpected(std::format(
				"Material thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.AssetPath.ToString()));
		}
		std::vector<::Durin::Editor::FAssetThumbnailPackageFingerprint> Dependencies;
		Dependencies.reserve(Closure.Assets.size() - 1);
		for (const FAssetData& Data : Closure.Assets)
			if (Data.PackagePath != Request.Asset.PackagePath)
				Dependencies.push_back(MakeFingerprint(Data));

		const ::Durin::Editor::FThumbnailVisualContract Visual;
		GenerationRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				::Durin::Editor::FThumbnailVisualContract::SphereAssetPath),
			.PreviewFixtureVersion =
				::Durin::Editor::FThumbnailVisualContract::SphereFixtureVersion,
			.ShaderContractVersion = MaterialThumbnailShaderContract,
			.Dependencies = std::move(Dependencies)};
		GenerationRequest.Input =
			std::make_shared<FMaterialThumbnailGenerationInput>(Request.Asset.AssetPath);
		GenerationRequest.RendererGeneration = RendererGeneration;
		GenerationRequest.RequestSerial = Request.RequestSerial;
		return GenerationRequest;
	}

	auto DMaterialThumbnailRenderer::CreateGenerationSession(
		const ::Durin::Editor::FAssetThumbnailGenerationRequest&,
		const ::Durin::Editor::IAssetThumbnailGenerationInput& Input)
		-> std::expected<std::unique_ptr<::Durin::Editor::IThumbnailRendererSession>, std::string>
	{
		const auto* MaterialInput = dynamic_cast<const FMaterialThumbnailGenerationInput*>(&Input);
		if (MaterialInput == nullptr)
		{
			return std::unexpected("The material thumbnail generation input is invalid.");
		}
		return std::make_unique<FMaterialThumbnailGenerationSession>(
			MaterialInput->AssetPath, AssetClassName);
	}

} // namespace Durin::Editor::Material
