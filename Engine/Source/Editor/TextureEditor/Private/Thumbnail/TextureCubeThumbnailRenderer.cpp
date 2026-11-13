#include "Engine/StreamableManager.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "DObject/WeakObjectPtr.h"

#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "Math/Operations.h"
#include "Texture/TextureCube.h"
#include "RenderResource.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"

namespace Durin::TextureEditor
{
	namespace
	{
		constexpr uint32 TextureCubeThumbnailGeneratorSchema = 2;
		constexpr uint32 TextureCubeThumbnailShaderContract = 2;

		auto MakeFingerprint(const FPackageData& Data, const FAssetData& Asset)
			-> FAssetThumbnailPackageFingerprint
		{
			return {
				.AssetPath = Asset.AssetPath,
				.PackagePath = Data.PackagePath,
				.AssetClassName = Asset.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

		auto CheckTextureCubeReadiness(
			DTextureCube* TextureCube) -> std::expected<bool, std::string>
		{
			if (TextureCube == nullptr || TextureCube->IsPendingKill())
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
			if (TextureCube->HasPendingRenderResourceInitialization()) return false;
			const auto* Resource = TextureCube->GetResource();
			if (!Resource || !Resource->GetTextureRHI_GameThread())
			{
				return std::unexpected("The TextureCube render resource is unavailable.");
			}
			return true;
		}

		auto MakeTextureCubeThumbnailView() -> FThumbnailPreviewView
		{
			const FThumbnailVisualContract Contract;
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
			: public IThumbnailRendererSession
		{
		public:
			explicit FTextureCubeThumbnailGenerationSession(DThumbnailRenderer& InRenderer, FTopLevelAssetPath InAssetPath)
				: Renderer(InRenderer), AssetPath(std::move(InAssetPath))
			{
			}

			~FTextureCubeThumbnailGenerationSession() override { ResetPreview(); }

			auto Load() -> FThumbnailRendererSessionUpdate override
			{
				ResetPreview();
				AssetLoad = StreamableManager.RequestAsyncLoad(AssetPath, {}, DTextureCube::StaticClass());
				return {.State = EThumbnailRendererSessionState::WaitingForResources};
			}

			auto FinishLoad() -> FThumbnailRendererSessionUpdate
			{
				const bool Result = !AssetLoad->HasError();
				TextureCube = Result ? Cast<DTextureCube>(AssetLoad->GetLoadedAsset()) : nullptr;
				if (!Result || !TextureCube.IsValid())
				{
					TextureCube = nullptr;
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = AssetLoad->GetError().Message.empty()
							? "The requested asset is not a TextureCube."
							: AssetLoad->GetError().Message};
				}
				AssetRevision = TextureCube.Get()->GetPackage() ? TextureCube.Get()->GetPackage()->GetEditRevision() : 0;
				SourceIdentity = TextureCube.Get()->GetSource().GetIdentity();
				bAssetLoaded = true;
				return {
					.State = EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PollResources() -> FThumbnailRendererSessionUpdate override
			{
				if (!AssetLoad) return {.Diagnostic = "The thumbnail load was reset."};
				if (!AssetLoad->IsComplete())
					return {.State = EThumbnailRendererSessionState::WaitingForResources};
				if (!bAssetLoaded)
				{
					const auto Loaded = FinishLoad();
					if (Loaded.State == EThumbnailRendererSessionState::Failed) return Loaded;
				}
				auto Readiness = CheckTextureCubeReadiness(TextureCube.Get());
				if (!Readiness)
					return {
						.State = EThumbnailRendererSessionState::Failed,
						.Diagnostic = std::move(Readiness.error())};
				if (*Readiness && !bShadersRequested && GEngine && GEngine->GetRendererModule())
				{
					Shaders = GEngine->GetRendererModule()->PrepareSceneThumbnailShaders();
					bShadersRequested = true;
				}
				auto ShaderReady = Shaders.Poll();
				if (!ShaderReady) return {.Diagnostic = ShaderReady.error()};
				return {
					.State = *Readiness && *ShaderReady
						? EThumbnailRendererSessionState::ReadyToRender
						: EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PreparePreview(
				IThumbnailRenderContext& Context) -> std::expected<void, std::string> override
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
				Snapshot = TextureCube.Get()->GetResource()->GetTextureRHI_GameThread();
				const FRHITextureReferenceRef TextureReference = Snapshot
					? FTextureReference(Snapshot).GetTextureReferenceRHI() : FRHITextureReferenceRef{};
				if (TextureReference == nullptr)
				{
					return std::unexpected(std::format(
						"The rendered-thumbnail TextureCube {} has no texture reference.",
						AssetPath.ToString()));
				}
				if (auto Bound = Context.SetScene(Renderer.GetPreviewScene()); !Bound) return Bound;
				if (auto ViewResult = Context.SetView(MakeTextureCubeThumbnailView()); !ViewResult)
					return ViewResult;
				return Context.SetViewEnvironment({.TextureReference = TextureReference});
			}

			auto ValidatePreparedInput() const -> std::expected<void, std::string> override
			{
				auto Readiness = CheckTextureCubeReadiness(TextureCube.Get());
				if (!Readiness) return std::unexpected(std::move(Readiness.error()));
				if (!*Readiness || !TextureCube.IsValid()
					|| (TextureCube.Get()->GetPackage() ? TextureCube.Get()->GetPackage()->GetEditRevision() : 0) != AssetRevision
					|| TextureCube.Get()->GetSource().GetIdentity() != SourceIdentity
					|| !Snapshot || TextureCube.Get()->GetResource()->GetTextureRHI_GameThread() != Snapshot)
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
				if (AssetLoad) AssetLoad->CancelHandle();
				AssetLoad.reset();
				Shaders = {};
				bShadersRequested = false;
			}

		private:
			FShaderMapPreparation Shaders;
			bool bShadersRequested = false;
			FStreamableManager StreamableManager;
			std::shared_ptr<FStreamableHandle> AssetLoad;
			bool bAssetLoaded = false;
			FXxHash128 SourceIdentity{};
			FTextureRHIRef Snapshot;
			DThumbnailRenderer& Renderer;
			FTopLevelAssetPath AssetPath;
			TWeakObjectPtr<DTextureCube> TextureCube;
			uint64 AssetRevision = 0;
		};
	} // namespace

	DTextureCubeThumbnailRenderer::DTextureCubeThumbnailRenderer(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DTextureCubeThumbnailRenderer::GetRegistration() const
		-> FThumbnailRenderingInfo
	{
		return {
			.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.RendererName = "TextureCubeRenderedThumbnail",
			.GeneratorSchemaVersion = TextureCubeThumbnailGeneratorSchema};
	}

	auto DTextureCubeThumbnailRenderer::CaptureGenerationRequest(
		const FAssetThumbnailRequest& Request,
		uint64 RendererGeneration) -> std::expected<FAssetThumbnailGenerationRequest, std::string>
	{
		FAssetThumbnailGenerationRequest GenerationRequest;

		const FThumbnailRenderingInfo Registration = GetRegistration();
		if (Request.Asset.AssetClassName != Registration.AssetClassName)
		{
			return std::unexpected("The TextureCube thumbnail renderer received the wrong asset class.");
		}
		const FAssetCatalogEntry Entry = FindAssetExact(Request.Asset.AssetPath, EAssetQuerySource::OnDisk);
		if (!Entry || Entry.Package->PackagePath != Request.Asset.PackagePath)
		{
			return std::unexpected(std::format(
				"TextureCube thumbnail registry data is missing for {}.",
				Request.Asset.AssetPath.ToString()));
		}
		if (MakeFingerprint(*Entry.Package, *Entry.Asset) != Request.Asset)
		{
			return std::unexpected(std::format(
				"TextureCube thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.AssetPath.ToString()));
		}

		const FThumbnailVisualContract Visual;
		GenerationRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity =
				std::string(
					FThumbnailVisualContract::TextureCubeEnvironmentViewIdentity),
			.PreviewFixtureVersion =
				FThumbnailVisualContract::TextureCubeEnvironmentViewVersion,
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
		const FAssetThumbnailGenerationRequest&,
		const IAssetThumbnailGenerationInput& Input)
		-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string>
	{
		const auto* TextureCubeInput =
			dynamic_cast<const FTextureCubeThumbnailGenerationInput*>(&Input);
		if (TextureCubeInput == nullptr)
		{
			return std::unexpected("The TextureCube thumbnail generation input is invalid.");
		}
		return std::make_unique<FTextureCubeThumbnailGenerationSession>(
			*this, TextureCubeInput->AssetPath);
	}
} // namespace Durin::TextureEditor
