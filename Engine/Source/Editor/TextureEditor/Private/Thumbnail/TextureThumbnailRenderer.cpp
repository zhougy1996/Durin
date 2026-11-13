#include "Engine/StreamableManager.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/TextureThumbnailRenderer.h"
#include "RenderResource.h"
#include "Shader/ShaderMapPreparation.h"

#include "Asset/Asset.h"
#include "Texture/Texture2D.h"
#include "Preview/TexturePreview.h"
#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin::TextureEditor
{
	namespace
	{
		class FTextureThumbnailGenerationInput final : public IAssetThumbnailGenerationInput
		{
		public:
			explicit FTextureThumbnailGenerationInput(FTopLevelAssetPath Path) : AssetPath(std::move(Path)) {}
			FTopLevelAssetPath AssetPath;
		};

		class FTextureThumbnailSession final : public IThumbnailRendererSession
		{
		public:
			explicit FTextureThumbnailSession(FTopLevelAssetPath Path) : AssetPath(std::move(Path)) {}
			~FTextureThumbnailSession() override { ResetPreview(); }

			auto Update() -> FThumbnailRendererSessionUpdate override
			{
				if (!bStarted)
				{
					bStarted = true;
					AssetLoad = StreamableManager.RequestAsyncLoad(AssetPath, {}, DTexture2D::StaticClass());
					Shaders = FShaderMapPreparation::Request("DurinEd", {"DurinEd.TexturePreview"}, EShaderBuildPriority::High);
					return {.State = EThumbnailRendererSessionState::WaitingForResources};
				}

				if (!AssetLoad) return {.Diagnostic = "The thumbnail load was reset."};
				if (!AssetLoad->IsComplete()) return {.State = EThumbnailRendererSessionState::WaitingForResources};
				if (AssetLoad->HasError()) return {.Diagnostic = (AssetLoad->GetError().Message)};
				if (!bAssetLoaded)
				{
					Texture = Cast<DTexture2D>(AssetLoad->GetLoadedAsset());
					if (!Texture.IsValid()) return {.Diagnostic = "Texture2D asset could not be loaded."};
					if (!Texture.Get()->IsAsyncCacheComplete())
						return {.State = EThumbnailRendererSessionState::WaitingForResources};
					AssetRevision = Texture.Get()->GetPackage() ? Texture.Get()->GetPackage()->GetEditRevision() : 0;
					Platform = Texture.Get()->GetPlatformDataShared();
					Options.Usage = Texture.Get()->GetUsage();
					bAssetLoaded = true;
				}
				if (!Texture.IsValid()) return {.Diagnostic = "Texture2D asset is unavailable."};
				if (!Texture.Get()->IsAsyncCacheComplete() || Texture.Get()->HasPendingRenderResourceInitialization())
					return {.State = EThumbnailRendererSessionState::WaitingForResources};
				const auto* Resource = Texture.Get()->GetResource();
				if (!Resource || !Resource->GetTextureRHI_GameThread())
					return {.Diagnostic = "Texture2D built render resource is unavailable."};
				auto ShaderReady = Shaders.Poll();
				if (!ShaderReady) return {.Diagnostic = ShaderReady.error()};
				return {.State = *ShaderReady ? EThumbnailRendererSessionState::ReadyToRender
					: EThumbnailRendererSessionState::WaitingForResources};
			}
			auto PreparePreview(IThumbnailRenderContext& Context) -> std::expected<void, std::string> override
			{
				const auto* Asset = Texture.Get();
				if (!Asset || !Asset->IsAsyncCacheComplete() || Asset->HasPendingRenderResourceInitialization())
					return std::unexpected("Texture2D render resource is not ready.");
				const auto* Resource = Asset->GetResource();
				Snapshot = Resource ? Resource->GetTextureRHI_GameThread() : FTextureRHIRef{};
				if (!Snapshot) { return std::unexpected("Texture2D built allocation is unavailable."); }
				Context.SetImageRenderer([Input = Snapshot, Display = Options](
					FRHICommandListImmediate& Commands, uint32 Width, uint32 Height) {
					return RenderTexturePreview(Commands, Input, Width, Height, Display, true);
				});
				return {};
			}
			auto ValidatePreparedInput() const -> std::expected<void, std::string> override
			{
				if (!Texture.IsValid() || !Texture.Get()->IsAsyncCacheComplete()
					|| Texture.Get()->HasPendingRenderResourceInitialization() || !Snapshot
					|| !Texture.Get()->GetResource()
					|| Texture.Get()->GetResource()->GetTextureRHI_GameThread() != Snapshot
					|| Texture.Get()->GetPlatformDataShared() != Platform || Texture.Get()->GetUsage() != Options.Usage
					|| (Texture.Get()->GetPackage() ? Texture.Get()->GetPackage()->GetEditRevision() : 0) != AssetRevision)
				{
					return std::unexpected("Texture2D changed while its thumbnail was being generated.");
				}
				return {};
			}
			auto ResetPreview() -> void override
			{
				bStarted = true;
				Snapshot = nullptr;
				Texture = nullptr;
				Platform.reset();
				bAssetLoaded = false;
				if (AssetLoad) AssetLoad->CancelHandle();
				AssetLoad.reset();
				Shaders = {};
			}
		private:
			bool bStarted = false;
			FShaderMapPreparation Shaders;
			FStreamableManager StreamableManager;
			std::shared_ptr<FStreamableHandle> AssetLoad;
			bool bAssetLoaded = false;
			FTopLevelAssetPath AssetPath;
			TWeakObjectPtr<DTexture2D> Texture;
			std::shared_ptr<const FTexturePlatformData> Platform;
			FTexturePreviewOptions Options;
			FTextureRHIRef Snapshot;
			uint64 AssetRevision = 0;
		};
	}

	DTextureThumbnailRenderer::DTextureThumbnailRenderer(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DTextureThumbnailRenderer::GetRegistration() const
		-> FThumbnailRenderingInfo
	{
		return {
			.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
			.RendererName = "Texture2DBuiltThumbnail",
			.GeneratorSchemaVersion = 2};
	}

	auto DTextureThumbnailRenderer::CaptureGenerationInput(const FAssetThumbnailPackageFingerprint& Asset) -> std::expected<FThumbnailGenerationData, std::string>
	{
		FThumbnailGenerationData Result;

		if (Asset.AssetClassName != GetRegistration().AssetClassName)
		{
			return std::unexpected("The Texture2D thumbnail renderer received the wrong asset class.");
		}
		const FAssetCatalogEntry Entry =
			FindAssetExact(Asset.AssetPath, EAssetQuerySource::OnDisk);
		if (!Entry || Entry->AssetClassName != Asset.AssetClassName
			|| Entry.Package->FormatVersion != Asset.PackageFormatVersion
			|| static_cast<uint64>(Entry.Package->FileSize) != Asset.FileSize
			|| Entry.Package->LastWriteTimeTicks != Asset.LastWriteTimeTicks)
		{
			return std::unexpected("Texture2D thumbnail registry data is missing or changed.");
		}
		// Capture package identity only. Loading and GPU readiness belong to the
		// scheduled cold-miss session; warm cache hits never inspect source pixels.

		Result.Output = {.Width = 256, .Height = 256};
		Result.PreviewFixtureIdentity = "Texture2D.Built.Auto.RGBA";
		Result.PreviewFixtureVersion = 1;
		Result.ShaderContractVersion = 2;
		Result.Input = std::make_shared<FTextureThumbnailGenerationInput>(Asset.AssetPath);

		Result.bHasTransparency = true;
		return Result;
	}

	auto DTextureThumbnailRenderer::CreateGenerationSession(const IAssetThumbnailGenerationInput& Input)
		-> std::unique_ptr<IThumbnailRendererSession>
	{
		const auto* Typed = dynamic_cast<const FTextureThumbnailGenerationInput*>(&Input);
		checkf(Typed != nullptr, "Invalid Texture2D thumbnail input.");
		return std::make_unique<FTextureThumbnailSession>(Typed->AssetPath);
	}
} // namespace Durin::TextureEditor
