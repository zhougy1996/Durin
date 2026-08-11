#include "Thumbnail/TextureCubeAssetThumbnail.h"

#include "AssetSystem.h"
#include "Math/Operations.h"
#include "Texture/TextureCube.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		constexpr uint32 TextureCubeThumbnailGeneratorSchema = 2;
		constexpr uint32 TextureCubeThumbnailShaderContract = 2;

		auto MakeFingerprint(const Asset::FAssetData& Data)
			-> ::Durin::Editor::FAssetThumbnailPackageFingerprint
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

		auto MakeTextureCubeThumbnailView() -> ::Durin::Editor::FRenderedAssetThumbnailPreviewView
		{
			const ::Durin::Editor::FRenderedAssetThumbnailVisualContract Contract;
			const float EnvironmentYScale = 1.0f / std::tan(
				Math::DegreesToRadians(
					FTextureCubeAssetThumbnailVisualContract::VerticalFieldOfViewDegrees)
				* 0.5f);
			const double CompatibleVerticalFieldOfViewDegrees =
				Math::RadiansToDegrees(
					2.0 * std::atan(1.0 / static_cast<double>(EnvironmentYScale)));
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
				// Reconstruct the legacy float-quantized 100-degree projection exactly.
				.VerticalFieldOfViewDegrees = CompatibleVerticalFieldOfViewDegrees,
				.NearClipDistance = Contract.NearClipDistance,
				.FarClipDistance = Contract.FarClipDistance,
				.ClearRed = Contract.BackgroundRed,
				.ClearGreen = Contract.BackgroundGreen,
				.ClearBlue = Contract.BackgroundBlue,
				.ClearAlpha = 1.0f};
		}

		class FTextureCubeThumbnailGenerationSession final
			: public ::Durin::Editor::IRenderedAssetThumbnailGenerationSession
		{
		public:
			explicit FTextureCubeThumbnailGenerationSession(FAssetPath InAssetPath)
				: AssetPath(std::move(InAssetPath))
			{
			}

			auto Load() -> ::Durin::Editor::FRenderedAssetThumbnailSessionUpdate override
			{
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Loaded);
				TextureCube = Result ? Cast<DTextureCube>(Loaded) : nullptr;
				if (!Result || TextureCube == nullptr)
				{
					TextureCube = nullptr;
					return {
						.State = ::Durin::Editor::ERenderedAssetThumbnailSessionState::Failed,
						.Diagnostic = Result.Message.empty()
							? "The requested asset is not a TextureCube."
							: Result.Message};
				}
				AssetRevision = TextureCube->GetBuildRevision();
				return {
					.State = ::Durin::Editor::ERenderedAssetThumbnailSessionState::WaitingForResources,
					.AssetRevision = AssetRevision};
			}

			auto PollResources() -> ::Durin::Editor::FRenderedAssetThumbnailSessionUpdate override
			{
				bool bReady = false;
				std::string Error;
				const uint64 Revision = GetTextureCubeResourceRevision(
					TextureCube, bReady, Error);
				if (!Error.empty())
					return {
						.State = ::Durin::Editor::ERenderedAssetThumbnailSessionState::Failed,
						.AssetRevision = AssetRevision,
						.ResourceRevision = Revision,
						.Diagnostic = std::move(Error)};
				return {
					.State = bReady
						? ::Durin::Editor::ERenderedAssetThumbnailSessionState::ReadyToRender
						: ::Durin::Editor::ERenderedAssetThumbnailSessionState::WaitingForResources,
					.AssetRevision = AssetRevision,
					.ResourceRevision = Revision};
			}

			auto PreparePreview(
				::Durin::Editor::IRenderedAssetThumbnailPreviewScene& PreviewScene,
				std::string& OutError) -> bool override
			{
				if (TextureCube == nullptr)
				{
					OutError = std::format(
						"The rendered-thumbnail TextureCube {} is unavailable.",
						AssetPath.ToString());
					return false;
				}
				const FRHITextureReferenceRef TextureReference =
					TextureCube->GetTextureReferenceRHI();
				if (TextureReference == nullptr)
				{
					OutError = std::format(
						"The rendered-thumbnail TextureCube {} has no texture reference.",
						AssetPath.ToString());
					return false;
				}
				return PreviewScene.SetView(MakeTextureCubeThumbnailView(), OutError)
					&& PreviewScene.SetViewEnvironment(
						{.TextureReference = TextureReference}, OutError);
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

			auto ResetPreview() -> void override {}

		private:
			FAssetPath AssetPath;
			DTextureCube* TextureCube = nullptr;
			uint64 AssetRevision = 0;
		};
	} // namespace

	auto FTextureCubeAssetThumbnailProvider::GetRegistration() const
		-> ::Durin::Editor::FAssetThumbnailProviderRegistration
	{
		return {
			.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.ProviderName = "TextureCubeRenderedThumbnail",
			.GeneratorSchemaVersion = TextureCubeThumbnailGeneratorSchema};
	}

	auto FTextureCubeAssetThumbnailProvider::CaptureGenerationRequest(
		const ::Durin::Editor::FAssetThumbnailRequest& Request,
		uint64 ProviderGeneration,
		::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		const ::Durin::Editor::FAssetThumbnailProviderRegistration Registration = GetRegistration();
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

		const ::Durin::Editor::FRenderedAssetThumbnailVisualContract Visual;
		OutRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity =
				std::string(
					::Durin::Editor::FRenderedAssetThumbnailVisualContract::
						TextureCubeEnvironmentViewIdentity),
			.PreviewFixtureVersion =
				::Durin::Editor::FRenderedAssetThumbnailVisualContract::
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
		const ::Durin::Editor::FAssetThumbnailGenerationRequest&,
		const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
		std::string& OutError)
		-> std::unique_ptr<::Durin::Editor::IRenderedAssetThumbnailGenerationSession>
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
} // namespace Durin::Editor::Texture
