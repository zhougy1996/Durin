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
			DTextureCube* TextureCube) -> std::expected<bool, std::string>
		{
			if (TextureCube == nullptr)
			{
				return std::unexpected("The TextureCube asset is unavailable.");
			}
			if (!TextureCube->IsAsyncCacheComplete()) return false;
			if (!TextureCube->HasPlatformData())
			{
				return std::unexpected("The TextureCube has no installed platform data.");
			}
			if (TextureCube->GetTextureReferenceRHI() == nullptr)
			{
				return std::unexpected("The TextureCube has no texture reference.");
			}
			const auto State = TextureCube->GetResourceUpdateState();
			if (TextureCube->IsResourceUpdatePending()) return false;
			if (State == ETextureResourceUpdateState::Failed && !TextureCube->HasUsableResource())
			{
				return std::unexpected("The TextureCube render resource failed.");
			}
			if (State == ETextureResourceUpdateState::Closed)
			{
				return std::unexpected("The TextureCube render resource was released.");
			}
			return TextureCube->HasUsableResource();
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
						.Diagnostic = (Result ? std::string{} : Result.error().Message).empty()
							? "The requested asset is not a TextureCube."
							: (Result ? std::string{} : Result.error().Message)};
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
				auto Readiness = CheckTextureCubeReadiness(TextureCube.Get());
				if (!Readiness)
					return {
						.State = ::Durin::Editor::EThumbnailRendererSessionState::Failed,
						.Diagnostic = std::move(Readiness.error())};
				return {
					.State = *Readiness
						? ::Durin::Editor::EThumbnailRendererSessionState::ReadyToRender
						: ::Durin::Editor::EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PreparePreview(
				::Durin::Editor::IThumbnailPreviewScene& PreviewScene) -> std::expected<void, std::string> override
			{
				if (!TextureCube.IsValid())
				{
					return std::unexpected(std::format(
						"The rendered-thumbnail TextureCube {} is unavailable.",
						AssetPath.ToString()));
				}
				auto Readiness = CheckTextureCubeReadiness(TextureCube.Get());
				if (!Readiness) return std::unexpected(std::move(Readiness.error()));
				if (!*Readiness) return std::unexpected("The TextureCube render resource is not ready.");
				Snapshot = TextureCube.Get()->GetPublishedTexture();
				const FRHITextureReferenceRef TextureReference = Snapshot
					? FTextureReference(Snapshot).GetTextureReferenceRHI() : FRHITextureReferenceRef{};
				if (TextureReference == nullptr)
				{
					return std::unexpected(std::format(
						"The rendered-thumbnail TextureCube {} has no texture reference.",
						AssetPath.ToString()));
				}
				if (auto ViewResult = PreviewScene.SetView(MakeTextureCubeThumbnailView()); !ViewResult)
					return ViewResult;
				return PreviewScene.SetViewEnvironment({.TextureReference = TextureReference});
			}

			auto ValidatePreparedInput() const -> std::expected<void, std::string> override
			{
				auto Readiness = CheckTextureCubeReadiness(TextureCube.Get());
				if (!Readiness) return std::unexpected(std::move(Readiness.error()));
				if (!*Readiness || !TextureCube.IsValid()
					|| (TextureCube.Get()->GetPackage() ? TextureCube.Get()->GetPackage()->GetEditRevision() : 0) != AssetRevision
					|| TextureCube.Get()->GetSource().GetIdentity() != SourceIdentity
					|| !Snapshot || TextureCube.Get()->GetPublishedTexture() != Snapshot)
				{
					return std::unexpected("The TextureCube changed while its thumbnail was being generated.");
				}
				return {};
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
		uint64 RendererGeneration) -> std::expected<::Durin::Editor::FAssetThumbnailGenerationRequest, std::string>
	{
		::Durin::Editor::FAssetThumbnailGenerationRequest GenerationRequest;

		const ::Durin::Editor::FThumbnailRenderingInfo Registration = GetRegistration();
		if (Request.Asset.AssetClassName != Registration.AssetClassName)
		{
			return std::unexpected("The TextureCube thumbnail renderer received the wrong asset class.");
		}
		const FAssetCatalogEntry Entry =
			FindAssetExact(Request.Asset.PackagePath);
		const FAssetData* Data = Entry.Data ? &*Entry.Data : nullptr;
		if (Data == nullptr)
		{
			return std::unexpected(std::format(
				"TextureCube thumbnail registry data is missing for {}.",
				Request.Asset.AssetPath.ToString()));
		}
		if (MakeFingerprint(*Data, Request.Asset.AssetPath) != Request.Asset)
		{
			return std::unexpected(std::format(
				"TextureCube thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.AssetPath.ToString()));
		}

		const ::Durin::Editor::FThumbnailVisualContract Visual;
		GenerationRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity =
				std::string(
					::Durin::Editor::FThumbnailVisualContract::
						TextureCubeEnvironmentViewIdentity),
			.PreviewFixtureVersion =
				::Durin::Editor::FThumbnailVisualContract::
					TextureCubeEnvironmentViewVersion,
			.ShaderContractVersion = TextureCubeThumbnailShaderContract};
		GenerationRequest.Input =
			std::make_shared<FTextureCubeThumbnailGenerationInput>(
				Request.Asset.AssetPath);
		GenerationRequest.RendererGeneration = RendererGeneration;
		GenerationRequest.RequestSerial = Request.RequestSerial;
		GenerationRequest.bHasTransparency = false;
		return GenerationRequest;
	}

	auto DTextureCubeThumbnailRenderer::CreateGenerationSession(
		const ::Durin::Editor::FAssetThumbnailGenerationRequest&,
		const ::Durin::Editor::IAssetThumbnailGenerationInput& Input)
		-> std::expected<std::unique_ptr<::Durin::Editor::IThumbnailRendererSession>, std::string>
	{
		const auto* TextureCubeInput =
			dynamic_cast<const FTextureCubeThumbnailGenerationInput*>(&Input);
		if (TextureCubeInput == nullptr)
		{
			return std::unexpected("The TextureCube thumbnail generation input is invalid.");
		}
		return std::make_unique<FTextureCubeThumbnailGenerationSession>(
			TextureCubeInput->AssetPath);
	}
} // namespace Durin::Editor::Texture
