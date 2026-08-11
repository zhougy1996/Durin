#include "Thumbnail/TextureCubeAssetThumbnail.h"

#include "Asset/AssetRetention.h"
#include "AssetSystem.h"
#include "Engine/Actor.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "Preview/TextureCubePreviewComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 TextureCubeThumbnailGeneratorSchema = 2;
		constexpr uint32 TextureCubeThumbnailShaderContract = 2;

		auto MakeFingerprint(const Asset::FAssetData& Data)
			-> Editor::FAssetThumbnailPackageFingerprint
		{
			return {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

		auto GetTextureCubeResourceRevision(
			DTextureCube* TextureCube,
			bool& bOutReady,
			std::string& OutError) -> uint64
		{
			bOutReady = false;
			OutError.clear();
			if (TextureCube == nullptr)
			{
				OutError = "The TextureCube asset is unavailable.";
				return 0;
			}
			if (TextureCube->GetBuildStatus() != ETextureBuildStatus::Ready)
			{
				OutError = TextureCube->GetLastBuildError();
				if (OutError.empty()) OutError = "The TextureCube is not built.";
				return 0;
			}
			if (TextureCube->GetTextureReferenceRHI() == nullptr)
			{
				OutError = "The TextureCube has no texture reference.";
				return 0;
			}
			const ERenderResourceState State = TextureCube->GetRenderResourceState();
			if (State == ERenderResourceState::Failed)
			{
				OutError = "The TextureCube render resource failed.";
				return 0;
			}
			if (State == ERenderResourceState::Released)
			{
				OutError = "The TextureCube render resource was released.";
				return 0;
			}
			bOutReady = State == ERenderResourceState::Ready;
			return TextureCube->GetBuildRevision();
		}

		auto MakeTextureCubePreviewView() -> Editor::FRenderedAssetThumbnailPreviewView
		{
			const Editor::FRenderedAssetThumbnailVisualContract Contract;
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
				.ClearRed = Contract.BackgroundRed,
				.ClearGreen = Contract.BackgroundGreen,
				.ClearBlue = Contract.BackgroundBlue,
				.ClearAlpha = 1.0f};
		}

		class FTextureCubeThumbnailGenerationSession final
			: public Editor::IRenderedAssetThumbnailGenerationSession
		{
		public:
			explicit FTextureCubeThumbnailGenerationSession(FAssetPath InAssetPath)
				: AssetPath(std::move(InAssetPath))
			{
			}

			~FTextureCubeThumbnailGenerationSession() override
			{
				ResetPreview();
			}

			auto Load() -> Editor::FRenderedAssetThumbnailSessionUpdate override
			{
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Loaded);
				TextureCube = Result ? Cast<DTextureCube>(Loaded) : nullptr;
				if (!Result || TextureCube == nullptr)
				{
					TextureCube = nullptr;
					return {
						.State = Editor::ERenderedAssetThumbnailSessionState::Failed,
						.Diagnostic = Result.Message.empty()
							? "The requested asset is not a TextureCube."
							: Result.Message};
				}
				AssetRevision = TextureCube->GetBuildRevision();
				return {
					.State = Editor::ERenderedAssetThumbnailSessionState::WaitingForResources,
					.AssetRevision = AssetRevision};
			}

			auto PollResources() -> Editor::FRenderedAssetThumbnailSessionUpdate override
			{
				bool bReady = false;
				std::string Error;
				const uint64 Revision = GetTextureCubeResourceRevision(
					TextureCube, bReady, Error);
				if (!Error.empty())
					return {
						.State = Editor::ERenderedAssetThumbnailSessionState::Failed,
						.AssetRevision = AssetRevision,
						.ResourceRevision = Revision,
						.Diagnostic = std::move(Error)};
				return {
					.State = bReady
						? Editor::ERenderedAssetThumbnailSessionState::ReadyToRender
						: Editor::ERenderedAssetThumbnailSessionState::WaitingForResources,
					.AssetRevision = AssetRevision,
					.ResourceRevision = Revision};
			}

			auto PreparePreview(
				Editor::IRenderedAssetThumbnailPreviewScene& PreviewScene,
				std::string& OutError) -> bool override
			{
				ResetPreview();
				World = PreviewScene.GetWorld();
				FAssetPath SpherePath;
				if (World == nullptr
					|| !FAssetPath::TryCreate(
						Editor::FRenderedAssetThumbnailVisualContract::SphereVirtualPath,
						SpherePath,
						&OutError)
					|| !Editor::FAssetRetentionService::Acquire(SpherePath, SphereAsset, OutError))
					return false;
				DStaticMesh* Sphere = Cast<DStaticMesh>(SphereAsset.Get());
				Actor = World->SpawnActor<AActor>("TextureCubeThumbnailPreviewActor");
				Component = Actor
					? Cast<DTextureCubePreviewComponent>(Actor->AddInstanceComponent(
						DTextureCubePreviewComponent::StaticClass(), "TextureCubePreview"))
					: nullptr;
				if (Sphere == nullptr || Component == nullptr || TextureCube == nullptr
					|| TextureCube->GetTextureReferenceRHI() == nullptr)
				{
					OutError = "The rendered-thumbnail TextureCube preview is unavailable.";
					ResetPreview();
					return false;
				}
				Component->SetStaticMesh(Sphere);
				Component->SetTextureCube(TextureCube);
				if (!PreviewScene.SetView(MakeTextureCubePreviewView(), OutError))
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
				const uint64 Revision = GetTextureCubeResourceRevision(
					TextureCube, bReady, OutError);
				if (!OutError.empty()) return false;
				if (!bReady || TextureCube == nullptr
					|| TextureCube->GetBuildRevision() != ExpectedAssetRevision
					|| Revision != ExpectedResourceRevision)
				{
					OutError = "The TextureCube changed while its thumbnail was being generated.";
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
			DTextureCube* TextureCube = nullptr;
			uint64 AssetRevision = 0;
			DWorld* World = nullptr;
			AActor* Actor = nullptr;
			DTextureCubePreviewComponent* Component = nullptr;
			Editor::FRetainedAsset SphereAsset;
		};
	} // namespace

	auto FTextureCubeAssetThumbnailProvider::GetRegistration() const
		-> Editor::FAssetThumbnailProviderRegistration
	{
		return {
			.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.ProviderName = "TextureCubeRenderedThumbnail",
			.GeneratorSchemaVersion = TextureCubeThumbnailGeneratorSchema};
	}

	auto FTextureCubeAssetThumbnailProvider::CaptureGenerationRequest(
		const Editor::FAssetThumbnailRequest& Request,
		uint64 ProviderGeneration,
		Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		const Editor::FAssetThumbnailProviderRegistration Registration = GetRegistration();
		if (Request.Asset.AssetClassName != Registration.AssetClassName)
		{
			OutError = "The TextureCube thumbnail provider received the wrong asset class.";
			return false;
		}
		const Asset::FAssetData* Data =
			Asset::GetAssetRegistry().FindAssetExact(Request.Asset.VirtualPath);
		if (Data == nullptr)
		{
			OutError = std::format(
				"TextureCube thumbnail registry data is missing for {}.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}
		if (MakeFingerprint(*Data) != Request.Asset)
		{
			OutError = std::format(
				"TextureCube thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}

		const Editor::FRenderedAssetThumbnailVisualContract Visual;
		OutRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity =
				std::string(
					Editor::FRenderedAssetThumbnailVisualContract::
						TextureCubeEnvironmentViewIdentity),
			.PreviewFixtureVersion =
				Editor::FRenderedAssetThumbnailVisualContract::
					TextureCubeEnvironmentViewVersion,
			.ShaderContractVersion = TextureCubeThumbnailShaderContract};
		OutRequest.Input =
			std::make_shared<FTextureCubeThumbnailGenerationInput>(
				Request.Asset.VirtualPath);
		OutRequest.ProviderGeneration = ProviderGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		OutRequest.bHasTransparency = false;
		return true;
	}

	auto FTextureCubeAssetThumbnailProvider::CreateGenerationSession(
		const Editor::FAssetThumbnailGenerationRequest&,
		const Editor::IAssetThumbnailGenerationInput& Input,
		std::string& OutError)
		-> std::unique_ptr<Editor::IRenderedAssetThumbnailGenerationSession>
	{
		const auto* TextureCubeInput =
			dynamic_cast<const FTextureCubeThumbnailGenerationInput*>(&Input);
		if (TextureCubeInput == nullptr)
		{
			OutError = "The TextureCube thumbnail generation input is invalid.";
			return nullptr;
		}
		OutError.clear();
		return std::make_unique<FTextureCubeThumbnailGenerationSession>(
			TextureCubeInput->AssetPath);
	}
} // namespace Durin
