#include "Thumbnail/MaterialAssetThumbnail.h"

#include "Asset/AssetRetention.h"
#include "AssetSystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Math/Operations.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 MaterialThumbnailGeneratorSchema = 3;
		constexpr uint32 MaterialThumbnailShaderContract = 2;
		constexpr float MaterialThumbnailSphereScale = 1.65f;

		class FMaterialThumbnailGenerationInput final
			: public IAssetThumbnailGenerationInput
		{
		public:
			explicit FMaterialThumbnailGenerationInput(FAssetPath InAssetPath)
				: AssetPath(std::move(InAssetPath))
			{
			}

			FAssetPath AssetPath;
		};

		auto MakeFingerprint(const Asset::FAssetData& Data)
			-> FAssetThumbnailPackageFingerprint
		{
			return {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

		auto GetMaterialResourceRevision(
			DMaterialInterface* Material,
			bool& bOutReady,
			std::string& OutError) -> uint64
		{
			bOutReady = false;
			OutError.clear();
			if (Material == nullptr)
			{
				OutError = "The material asset is unavailable.";
				return 0;
			}
			if (auto* Instance = Cast<DMaterialInstance>(Material); Instance != nullptr
				&& Instance->GetParent() == nullptr)
			{
				OutError = "The material instance has no valid parent.";
				return 0;
			}

			uint64 Revision = Material->GetRenderStateVersion();
			DTexture2D* Texture = nullptr;
			if (!Material->GetTextureParameterValue(
					MaterialParameters::BaseColorTextureName(), Texture)
				|| Texture == nullptr)
			{
				bOutReady = true;
				return Revision;
			}
			if (Texture->GetBuildStatus() != ETextureBuildStatus::Ready)
			{
				OutError = Texture->GetLastBuildError().empty()
					? "A referenced material texture is not built."
					: Texture->GetLastBuildError();
				return 0;
			}
			const ERenderResourceState State = Texture->GetRenderResourceState();
			if (State == ERenderResourceState::Failed)
			{
				OutError = "A referenced material texture render resource failed.";
				return 0;
			}
			bOutReady = State == ERenderResourceState::Ready;
			Revision ^= Texture->GetBuildRevision() + 0x9e3779b97f4a7c15ull
				+ (Revision << 6) + (Revision >> 2);
			return Revision == 0 ? 1 : Revision;
		}

		auto MakeMaterialPreviewView() -> FRenderedAssetThumbnailPreviewView
		{
			const FRenderedAssetThumbnailVisualContract Contract;
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
			: public IRenderedAssetThumbnailGenerationSession
		{
		public:
			FMaterialThumbnailGenerationSession(
				FAssetPath InAssetPath,
				std::string InAssetClassName)
				: AssetPath(std::move(InAssetPath))
				, AssetClassName(std::move(InAssetClassName))
			{
			}

			~FMaterialThumbnailGenerationSession() override
			{
				ResetPreview();
			}

			auto Load() -> FRenderedAssetThumbnailSessionUpdate override
			{
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Loaded);
				Material = Result ? Cast<DMaterialInterface>(Loaded) : nullptr;
				if (!Result || Material == nullptr
					|| Material->GetClass()->GetQualifiedName().ToString() != AssetClassName)
				{
					Material = nullptr;
					return {
						.State = ERenderedAssetThumbnailSessionState::Failed,
						.Diagnostic = Result.Message.empty()
							? "The requested asset is not an exact material class."
							: Result.Message};
				}
				AssetRevision = Material->GetRenderStateVersion();
				return {
					.State = ERenderedAssetThumbnailSessionState::WaitingForResources,
					.AssetRevision = AssetRevision};
			}

			auto PollResources() -> FRenderedAssetThumbnailSessionUpdate override
			{
				bool bReady = false;
				std::string Error;
				const uint64 Revision = GetMaterialResourceRevision(Material, bReady, Error);
				if (!Error.empty())
					return {
						.State = ERenderedAssetThumbnailSessionState::Failed,
						.AssetRevision = AssetRevision,
						.ResourceRevision = Revision,
						.Diagnostic = std::move(Error)};
				return {
					.State = bReady
						? ERenderedAssetThumbnailSessionState::ReadyToRender
						: ERenderedAssetThumbnailSessionState::WaitingForResources,
					.AssetRevision = AssetRevision,
					.ResourceRevision = Revision};
			}

			auto PreparePreview(
				IRenderedAssetThumbnailPreviewScene& PreviewScene,
				std::string& OutError) -> bool override
			{
				ResetPreview();
				World = PreviewScene.GetWorld();
				FAssetPath SpherePath;
				if (World == nullptr
					|| !FAssetPath::TryCreate(
						FRenderedAssetThumbnailVisualContract::SphereVirtualPath,
						SpherePath,
						&OutError)
					|| !Editor::FAssetRetentionService::Acquire(SpherePath, SphereAsset, OutError))
					return false;
				DStaticMesh* Sphere = Cast<DStaticMesh>(SphereAsset.Get());
				Actor = World->SpawnActor<AActor>("MaterialThumbnailPreviewActor");
				Component = Actor
					? Cast<DStaticMeshComponent>(Actor->AddInstanceComponent(
						DStaticMeshComponent::StaticClass(), "MaterialPreview"))
					: nullptr;
				if (Sphere == nullptr || Component == nullptr || Material == nullptr)
				{
					OutError = "The rendered-thumbnail material preview is unavailable.";
					ResetPreview();
					return false;
				}
				Component->SetStaticMesh(Sphere);
				for (uint32 SlotIndex = 0; SlotIndex < Component->GetNumMaterials(); ++SlotIndex)
					Component->SetMaterial(SlotIndex, Material);
				FTransform Transform;
				Transform.Scale3D = FVector3(MaterialThumbnailSphereScale);
				Component->SetWorldTransform(Transform);
				if (!PreviewScene.SetView(MakeMaterialPreviewView(), OutError))
				{
					ResetPreview();
					return false;
				}
				return true;
			}

			auto ValidateRevisions(
				uint64 ExpectedAssetRevision,
				uint64 ExpectedResourceRevision,
				std::string& OutError) const -> bool override
			{
				bool bReady = false;
				const uint64 Revision = GetMaterialResourceRevision(
					Material, bReady, OutError);
				if (!OutError.empty()) return false;
				if (!bReady || Material == nullptr
					|| Material->GetRenderStateVersion() != ExpectedAssetRevision
					|| Revision != ExpectedResourceRevision)
				{
					OutError = "The material changed while its thumbnail was being generated.";
					return false;
				}
				return true;
			}

			auto ResetPreview() -> void override
			{
				if (World != nullptr && Actor != nullptr) World->DestroyActor(Actor);
				Component = nullptr;
				Actor = nullptr;
				World = nullptr;
				SphereAsset = {};
			}

		private:
			FAssetPath AssetPath;
			std::string AssetClassName;
			DMaterialInterface* Material = nullptr;
			uint64 AssetRevision = 0;
			DWorld* World = nullptr;
			AActor* Actor = nullptr;
			DStaticMeshComponent* Component = nullptr;
			Editor::FRetainedAsset SphereAsset;
		};
	} // namespace

	FMaterialAssetThumbnailProvider::FMaterialAssetThumbnailProvider(
		std::string InAssetClassName)
		: AssetClassName(std::move(InAssetClassName))
	{
	}

	auto FMaterialAssetThumbnailProvider::GetRegistration() const
		-> FAssetThumbnailProviderRegistration
	{
		return {
			.AssetClassName = AssetClassName,
			.ProviderName = "MaterialRenderedThumbnail",
			.GeneratorSchemaVersion = MaterialThumbnailGeneratorSchema};
	}

	auto FMaterialAssetThumbnailProvider::CaptureGenerationRequest(
		const FAssetThumbnailRequest& Request,
		uint64 ProviderGeneration,
		FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		if (Request.Asset.AssetClassName != AssetClassName)
		{
			OutError = "The material thumbnail provider received the wrong asset class.";
			return false;
		}

		const Asset::FAssetRegistry& Registry = Asset::GetAssetRegistry();
		const Asset::FAssetData* Root = Registry.FindAssetExact(Request.Asset.VirtualPath);
		if (Root == nullptr)
		{
			OutError = std::format(
				"Material thumbnail registry data is missing for {}.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}
		if (MakeFingerprint(*Root) != Request.Asset)
		{
			OutError = std::format(
				"Material thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}
		std::vector<FAssetThumbnailDependencyNode> Nodes;
		Nodes.reserve(Registry.GetAssets().size());
		for (const auto& [Path, Data] : Registry.GetAssets())
		{
			Nodes.push_back({
				.Package = MakeFingerprint(Data),
				.Dependencies = Data.Dependencies});
		}
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
		if (!BuildAssetThumbnailDependencyClosure(
				Request.Asset.VirtualPath, Nodes, Dependencies, OutError))
			return false;

		const FRenderedAssetThumbnailVisualContract Visual;
		OutRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				FRenderedAssetThumbnailVisualContract::SphereVirtualPath),
			.PreviewFixtureVersion =
				FRenderedAssetThumbnailVisualContract::SphereFixtureVersion,
			.ShaderContractVersion = MaterialThumbnailShaderContract,
			.Dependencies = std::move(Dependencies)};
		OutRequest.Input =
			std::make_shared<FMaterialThumbnailGenerationInput>(Request.Asset.VirtualPath);
		OutRequest.ProviderGeneration = ProviderGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		return true;
	}

	auto FMaterialAssetThumbnailProvider::CreateGenerationSession(
		const FAssetThumbnailGenerationRequest&,
		const IAssetThumbnailGenerationInput& Input,
		std::string& OutError)
		-> std::unique_ptr<IRenderedAssetThumbnailGenerationSession>
	{
		const auto* MaterialInput = dynamic_cast<const FMaterialThumbnailGenerationInput*>(&Input);
		if (MaterialInput == nullptr)
		{
			OutError = "The material thumbnail generation input is invalid.";
			return nullptr;
		}
		OutError.clear();
		return std::make_unique<FMaterialThumbnailGenerationSession>(
			MaterialInput->AssetPath, AssetClassName);
	}

} // namespace Durin
