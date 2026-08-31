#include "Thumbnail/MaterialThumbnailRenderer.h"

#include "Asset/AssetRetention.h"
#include "Asset.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Package.h"
#include "Engine/Actor.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Math/Operations.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"

#include <unordered_set>

namespace Durin::Editor::Material
{
	namespace
	{
		constexpr uint32 MaterialThumbnailGeneratorSchema = 5;
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

		auto MakeFingerprint(const Asset::FAssetData& Data,
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
				OutError = Current == nullptr
					? "The material instance has no valid parent."
					: "The material instance parent chain contains a cycle.";
				return 0;
			}
			const FMaterialCompileStatus& CompileStatus =
				BaseMaterial->GetMaterialCompileStatus();
			if (!CompileStatus.IsCurrent()
				|| BaseMaterial->GetAcceptedCompiledProgram() == nullptr)
			{
				if (CompileStatus.State == EMaterialCompileState::Pending
					|| CompileStatus.State == EMaterialCompileState::Running)
					return Revision == 0 ? 1 : Revision;
				const auto Diagnostics = BaseMaterial->GetMaterialCompileDiagnostics();
				OutError = !Diagnostics.empty() && !Diagnostics.front().Source.Message.empty()
					? Diagnostics.front().Source.Message
					: "The material has no current compiled program.";
				return 0;
			}
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

		auto GetMaterialAssetRevision(
			DMaterialInterface* Material,
			std::string& OutError) -> uint64
		{
			OutError.clear();
			const DPackage* Package = Material ? Material->GetPackage() : nullptr;
			if (Package == nullptr)
			{
				OutError = "The material asset package is unavailable.";
				return 0;
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
				std::string SphereError;
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Result = Asset::LoadObject(AssetPath, Loaded);
				Material = Result ? Cast<DMaterialInterface>(Loaded) : nullptr;
				if (!Result || Material == nullptr
					|| Material->GetClass()->GetQualifiedName().ToString() != AssetClassName)
				{
					Material = nullptr;
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = Result.Message.empty()
							? "The requested asset is not an exact material class."
							: Result.Message};
				}
				if (auto* Instance = Cast<DMaterialInstance>(Material);
					Instance != nullptr && Instance->GetParent() == nullptr)
				{
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = "The material instance has no valid parent."};
				}
				FObjectPath SpherePath;
				if (!FObjectPath::TryCreate(
						::Durin::Editor::FThumbnailVisualContract::SphereAssetPath,
						SpherePath, &SphereError)
					|| !::Durin::Editor::FAssetRetentionService::Acquire(
						SpherePath, SphereAsset, SphereError)
					|| (Sphere = Cast<DStaticMesh>(SphereAsset.Get())) == nullptr)
				{
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = SphereError.empty()
							? "The rendered-thumbnail sphere mesh is unavailable."
							: std::move(SphereError)};
				}
				if (Sphere->GetRenderResourceStatus().Readiness
					== EStaticMeshRenderResourceReadiness::Unavailable)
					Sphere->InitResources();
				AssetRevision = GetMaterialAssetRevision(Material, SphereError);
				if (!SphereError.empty())
				{
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = std::move(SphereError)};
				}
				return {
					.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources,
					.AssetRevision = AssetRevision};
			}

			auto PollResources() -> ::Durin::Editor::FThumbnailRendererSessionUpdate override
			{
				bool bReady = false;
				std::string Error;
				const uint64 MaterialRevision =
					GetMaterialResourceRevision(Material, bReady, Error);
				if (!Error.empty())
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.AssetRevision = AssetRevision,
						.ResourceRevision = MaterialRevision,
						.Diagnostic = std::move(Error)};
				if (!bReady)
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources,
						.AssetRevision = AssetRevision,
						.ResourceRevision = MaterialRevision};
				if (Sphere == nullptr)
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.AssetRevision = AssetRevision,
						.Diagnostic = "The rendered-thumbnail sphere mesh is unavailable."};
				const FStaticMeshRenderResourceStatus SphereStatus =
					Sphere->GetRenderResourceStatus();
				if (SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Failed
					|| SphereStatus.Readiness == EStaticMeshRenderResourceReadiness::Unavailable)
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.AssetRevision = AssetRevision,
						.ResourceRevision = SphereStatus.Revision,
						.Diagnostic = "The rendered-thumbnail sphere render resource is unavailable."};
				const bool bSphereReady = SphereStatus.Readiness
					== EStaticMeshRenderResourceReadiness::Ready;
				const uint64 Revision = CombineResourceRevision(
					MaterialRevision, SphereStatus.Revision);
				return {
					.State = bReady && bSphereReady
						? ::Durin::Editor::EThumbnailRendererSessionState::ReadyToRender
						: ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources,
					.AssetRevision = AssetRevision,
					.ResourceRevision = Revision};
			}

			auto PreparePreview(
				::Durin::Editor::IThumbnailPreviewScene& PreviewScene,
				std::string& OutError) -> bool override
			{
				ResetScenePreview();
				World = PreviewScene.GetWorld();
				if (World == nullptr || Sphere == nullptr)
				{
					OutError = "The rendered-thumbnail material preview is unavailable.";
					return false;
				}
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
				const uint64 MaterialAssetRevision = GetMaterialAssetRevision(
					Material, OutError);
				if (!OutError.empty()) return false;
				bool bReady = false;
				const uint64 MaterialRevision = GetMaterialResourceRevision(
					Material, bReady, OutError);
				if (!OutError.empty()) return false;
				const FStaticMeshRenderResourceStatus SphereStatus = Sphere
					? Sphere->GetRenderResourceStatus()
					: FStaticMeshRenderResourceStatus{};
				const uint64 Revision = CombineResourceRevision(
					MaterialRevision, SphereStatus.Revision);
				if (!bReady || Material == nullptr
					|| SphereStatus.Readiness != EStaticMeshRenderResourceReadiness::Ready
					|| MaterialAssetRevision != ExpectedAssetRevision
					|| Revision != ExpectedResourceRevision)
				{
					OutError = "The material changed while its thumbnail was being generated.";
					return false;
				}
				return true;
			}

			auto ResetPreview() -> void override
			{
				ResetScenePreview();
				Sphere = nullptr;
				SphereAsset = {};
			}

		private:
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
			DWorld* World = nullptr;
			AActor* Actor = nullptr;
			DStaticMeshComponent* Component = nullptr;
			DStaticMesh* Sphere = nullptr;
			::Durin::Editor::FRetainedAsset SphereAsset;
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
		uint64 RendererGeneration,
		::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		if (Request.Asset.AssetClassName != AssetClassName)
		{
			OutError = "The material thumbnail renderer received the wrong asset class.";
			return false;
		}

		const Asset::FAssetDependencyClosureSnapshot Closure =
			Asset::CaptureAssetDependencyClosure(Request.Asset.PackagePath);
		if (!Closure)
		{
			OutError = Closure.Result.Message.empty()
				? std::format("Material thumbnail registry data is missing for {}.",
					Request.Asset.AssetPath.ToString())
				: Closure.Result.Message;
			return false;
		}
		const auto RootIt = std::ranges::find_if(
			Closure.Assets,
			[&Request](const Asset::FAssetData& Data) {
				return Data.PackagePath == Request.Asset.PackagePath;
			});
		if (RootIt == Closure.Assets.end())
		{
			OutError = "The material dependency closure omitted its root asset.";
			return false;
		}
		const Asset::FAssetData* Root = &*RootIt;
		if (MakeFingerprint(*Root, Request.Asset.AssetPath) != Request.Asset)
		{
			OutError = std::format(
				"Material thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.AssetPath.ToString());
			return false;
		}
		std::vector<::Durin::Editor::FAssetThumbnailPackageFingerprint> Dependencies;
		Dependencies.reserve(Closure.Assets.size() - 1);
		for (const Asset::FAssetData& Data : Closure.Assets)
			if (Data.PackagePath != Request.Asset.PackagePath)
				Dependencies.push_back(MakeFingerprint(Data));

		const ::Durin::Editor::FThumbnailVisualContract Visual;
		OutRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				::Durin::Editor::FThumbnailVisualContract::SphereAssetPath),
			.PreviewFixtureVersion =
				::Durin::Editor::FThumbnailVisualContract::SphereFixtureVersion,
			.ShaderContractVersion = MaterialThumbnailShaderContract,
			.Dependencies = std::move(Dependencies)};
		OutRequest.Input =
			std::make_shared<FMaterialThumbnailGenerationInput>(Request.Asset.AssetPath);
		OutRequest.RendererGeneration = RendererGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		return true;
	}

	auto DMaterialThumbnailRenderer::CreateGenerationSession(
		const ::Durin::Editor::FAssetThumbnailGenerationRequest&,
		const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
		std::string& OutError)
		-> std::unique_ptr<::Durin::Editor::IThumbnailRendererSession>
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

} // namespace Durin::Editor::Material
