#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "DObject/WeakObjectPtr.h"

#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "Math/Operations.h"
#include "Texture/TextureCube.h"
#include "RenderResource.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		constexpr uint32 TextureCubeThumbnailGeneratorSchema = 2;
		constexpr uint32 TextureCubeThumbnailShaderContract = 2;

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

		auto CheckTextureCubeReadiness(
			DTextureCube* TextureCube,
			bool& bOutReady,
			std::string& OutError) -> void
		{
			bOutReady = false;
			OutError.clear();
			if (TextureCube == nullptr)
			{
				OutError = "The TextureCube asset is unavailable.";
				return;
			}
			if (!TextureCube->HasPlatformData())
			{
				OutError = "The TextureCube has no installed platform data.";
				return;
			}
			if (TextureCube->GetTextureReferenceRHI() == nullptr)
			{
				OutError = "The TextureCube has no texture reference.";
				return;
			}
			const auto State = TextureCube->GetResourceUpdateState();
			if (TextureCube->IsResourceUpdatePending()) return;
			if (State == ETextureResourceUpdateState::Failed && !TextureCube->HasUsableResource())
			{
				OutError = "The TextureCube render resource failed.";
				return;
			}
			if (State == ETextureResourceUpdateState::Closed)
			{
				OutError = "The TextureCube render resource was released.";
				return;
			}
			bOutReady = TextureCube->HasUsableResource();
			return;
		}

		auto MakeTextureCubeThumbnailView() -> ::Durin::Editor::FThumbnailPreviewView
		{
			const ::Durin::Editor::FThumbnailVisualContract Contract;
			const float EnvironmentYScale = 1.0f / std::tan(
				Math::DegreesToRadians(
					FTextureCubeThumbnailRendererVisualContract::VerticalFieldOfViewDegrees)
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
			: public ::Durin::Editor::IThumbnailRendererSession
		{
		public:
			explicit FTextureCubeThumbnailGenerationSession(FTopLevelAssetPath InAssetPath)
				: AssetPath(std::move(InAssetPath))
			{
			}

			~FTextureCubeThumbnailGenerationSession() override { ResetPreview(); }

			auto Load() -> ::Durin::Editor::FThumbnailRendererSessionUpdate override
			{
				ResetPreview();
				AssetLoad = RequestAsyncLoad(AssetPath, {}, DTextureCube::StaticClass());
				return {.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
			}

			auto FinishLoad() -> ::Durin::Editor::FThumbnailRendererSessionUpdate
			{
				const auto& Result = AssetLoad->GetResult();
				TextureCube = Result ? Cast<DTextureCube>(AssetLoad->GetLoadedObject()) : nullptr;
				if (!Result || !TextureCube.IsValid())
				{
					TextureCube = nullptr;
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = Result.Message.empty()
							? "The requested asset is not a TextureCube."
							: Result.Message};
				}
				AssetRevision = TextureCube.Get()->GetPackage() ? TextureCube.Get()->GetPackage()->GetEditRevision() : 0;
				SourceIdentity = TextureCube.Get()->GetSource().GetIdentity();
				bAssetLoaded = true;
				return {
					.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PollResources() -> ::Durin::Editor::FThumbnailRendererSessionUpdate override
			{
				if (!AssetLoad) return {.Diagnostic = "The thumbnail load was reset."};
				if (!AssetLoad->IsComplete())
					return {.State = ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
				if (!bAssetLoaded)
				{
					const auto Loaded = FinishLoad();
					if (Loaded.State == ::Durin::Editor::EThumbnailRendererSessionState::Failed) return Loaded;
				}
				bool bReady = false;
				std::string Error;
				CheckTextureCubeReadiness(TextureCube.Get(), bReady, Error);
				if (!Error.empty())
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = std::move(Error)};
				return {
					.State = bReady
						? ::Durin::Editor::EThumbnailRendererSessionState::ReadyToRender
						: ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PreparePreview(
				::Durin::Editor::IThumbnailPreviewScene& PreviewScene,
				std::string& OutError) -> bool override
			{
				if (!TextureCube.IsValid())
				{
					OutError = std::format(
						"The rendered-thumbnail TextureCube {} is unavailable.",
						AssetPath.ToString());
					return false;
				}
				bool bReady = false;
				CheckTextureCubeReadiness(TextureCube.Get(), bReady, OutError);
				if (!bReady || !OutError.empty()) return false;
				Snapshot = TextureCube.Get()->GetPublishedTexture();
				const FRHITextureReferenceRef TextureReference = Snapshot
					? FTextureReference(Snapshot).GetTextureReferenceRHI() : FRHITextureReferenceRef{};
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

			auto ValidatePreparedInput(
				std::string& OutError) const -> bool override
			{
				bool bReady = false;
				CheckTextureCubeReadiness(
					TextureCube.Get(), bReady, OutError);
				if (!OutError.empty()) return false;
				if (!bReady || !TextureCube.IsValid()
					|| (TextureCube.Get()->GetPackage() ? TextureCube.Get()->GetPackage()->GetEditRevision() : 0) != AssetRevision
					|| TextureCube.Get()->GetSource().GetIdentity() != SourceIdentity
					|| !Snapshot || TextureCube.Get()->GetPublishedTexture() != Snapshot)
				{
					OutError = "The TextureCube changed while its thumbnail was being generated.";
					return false;
				}
				return true;
			}

			auto ResetPreview() -> void override
			{
				Snapshot = nullptr;
				TextureCube = nullptr;
				bAssetLoaded = false;
				if (AssetLoad) AssetLoad->Cancel();
				AssetLoad.reset();
			}

		private:
			std::shared_ptr<FAsyncLoadHandle> AssetLoad;
			bool bAssetLoaded = false;
			FXxHash128 SourceIdentity{};
			FTextureRHIRef Snapshot;
			FTopLevelAssetPath AssetPath;
			TWeakObjectPtr<DTextureCube> TextureCube;
			uint64 AssetRevision = 0;
		};
	} // namespace

	auto DTextureCubeThumbnailRenderer::GetRegistration() const
		-> ::Durin::Editor::FThumbnailRenderingInfo
	{
		return {
			.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.RendererName = "TextureCubeRenderedThumbnail",
			.GeneratorSchemaVersion = TextureCubeThumbnailGeneratorSchema};
	}

	auto DTextureCubeThumbnailRenderer::CaptureGenerationRequest(
		const ::Durin::Editor::FAssetThumbnailRequest& Request,
		uint64 RendererGeneration,
		::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		const ::Durin::Editor::FThumbnailRenderingInfo Registration = GetRegistration();
		if (Request.Asset.AssetClassName != Registration.AssetClassName)
		{
			OutError = "The TextureCube thumbnail renderer received the wrong asset class.";
			return false;
		}
		const FAssetCatalogEntry Entry =
			FindAssetExact(Request.Asset.PackagePath);
		const FAssetData* Data = Entry.Data ? &*Entry.Data : nullptr;
		if (Data == nullptr)
		{
			OutError = std::format(
				"TextureCube thumbnail registry data is missing for {}.",
				Request.Asset.AssetPath.ToString());
			return false;
		}
		if (MakeFingerprint(*Data, Request.Asset.AssetPath) != Request.Asset)
		{
			OutError = std::format(
				"TextureCube thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.AssetPath.ToString());
			return false;
		}

		const ::Durin::Editor::FThumbnailVisualContract Visual;
		OutRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity =
				std::string(
					::Durin::Editor::FThumbnailVisualContract::
						TextureCubeEnvironmentViewIdentity),
			.PreviewFixtureVersion =
				::Durin::Editor::FThumbnailVisualContract::
					TextureCubeEnvironmentViewVersion,
			.ShaderContractVersion = TextureCubeThumbnailShaderContract};
		OutRequest.Input =
			std::make_shared<FTextureCubeThumbnailGenerationInput>(
				Request.Asset.AssetPath);
		OutRequest.RendererGeneration = RendererGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		OutRequest.bHasTransparency = false;
		return true;
	}

	auto DTextureCubeThumbnailRenderer::CreateGenerationSession(
		const ::Durin::Editor::FAssetThumbnailGenerationRequest&,
		const ::Durin::Editor::IAssetThumbnailGenerationInput& Input,
		std::string& OutError)
		-> std::unique_ptr<::Durin::Editor::IThumbnailRendererSession>
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
