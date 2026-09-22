#include "Thumbnail/TextureThumbnailRenderer.h"

#include "Asset/Asset.h"
#include "Texture/Texture2D.h"
#include "TexturePreview.h"
#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin::Editor::Texture
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
			auto Load() -> FThumbnailRendererSessionUpdate override
			{
				ResetPreview();
				AssetLoad = RequestAsyncLoad(AssetPath, {}, DTexture2D::StaticClass());
				return {.State = EThumbnailRendererSessionState::WaitingForResources};
			}
			auto PollResources() -> FThumbnailRendererSessionUpdate override
			{
				if (!AssetLoad) return {.Diagnostic = "The thumbnail load was reset."};
				if (!AssetLoad->IsComplete()) return {.State = EThumbnailRendererSessionState::WaitingForResources};
				if (!AssetLoad->GetResult()) return {.Diagnostic = (AssetLoad->GetResult() ? std::string{} : AssetLoad->GetResult().error().Message)};
				if (!bAssetLoaded)
				{
					Texture = Cast<DTexture2D>(AssetLoad->GetLoadedObject());
					if (!Texture.IsValid()) return {.Diagnostic = "Texture2D asset could not be loaded."};
					if (!Texture.Get()->IsAsyncCacheComplete())
						return {.State = EThumbnailRendererSessionState::WaitingForResources};
					AssetRevision = Texture.Get()->GetPackage() ? Texture.Get()->GetPackage()->GetEditRevision() : 0;
					Platform = Texture.Get()->GetPlatformDataShared();
					Options.Usage = Texture.Get()->GetUsage();
					bAssetLoaded = true;
				}
				if (!Texture.IsValid()) return {.Diagnostic = "Texture2D asset is unavailable."};
				if (!Texture.Get()->IsAsyncCacheComplete() || Texture.Get()->IsResourceUpdatePending())
					return {.State = EThumbnailRendererSessionState::WaitingForResources};
				if (!Texture.Get()->HasUsableResource())
					return {.Diagnostic = "Texture2D built render resource is unavailable."};
				return {.State = EThumbnailRendererSessionState::ReadyToRender};
			}
			auto PreparePreview(IThumbnailPreviewScene& Scene) -> std::expected<void, std::string> override
			{
				Snapshot = Texture.IsValid() ? Texture.Get()->GetPublishedTexture() : FTextureRHIRef{};
				if (!Snapshot) { return std::unexpected("Texture2D built allocation is unavailable."); }
				return Scene.SetImageRenderer([Input = Snapshot, Display = Options](
					FRHICommandListImmediate& Commands, uint32 Width, uint32 Height) {
					return RenderTexturePreview(Commands, Input, Width, Height, Display);
				});
			}
			auto ValidatePreparedInput() const -> std::expected<void, std::string> override
			{
				if (!Texture.IsValid() || Texture.Get()->IsResourceUpdatePending() || !Snapshot
					|| !Texture.Get()->HasUsableResource() || Texture.Get()->GetPublishedTexture() != Snapshot
					|| Texture.Get()->GetPlatformDataShared() != Platform || Texture.Get()->GetUsage() != Options.Usage
					|| (Texture.Get()->GetPackage() ? Texture.Get()->GetPackage()->GetEditRevision() : 0) != AssetRevision)
				{
					return std::unexpected("Texture2D changed while its thumbnail was being generated.");
				}
				return {};
			}
			auto ResetPreview() -> void override
			{
				Snapshot = nullptr;
				Texture = nullptr;
				Platform.reset();
				bAssetLoaded = false;
				if (AssetLoad) AssetLoad->Cancel();
				AssetLoad.reset();
			}
		private:
			std::shared_ptr<FAsyncLoadHandle> AssetLoad;
			bool bAssetLoaded = false;
			FTopLevelAssetPath AssetPath;
			TWeakObjectPtr<DTexture2D> Texture;
			std::shared_ptr<const FTexturePlatformData> Platform;
			FTexturePreviewOptions Options;
			FTextureRHIRef Snapshot;
			uint64 AssetRevision = 0;
		};
	}

	auto DTextureThumbnailRenderer::GetRegistration() const
		-> ::Durin::Editor::FThumbnailRenderingInfo
	{
		return {
			.AssetClassName = DTexture2D::StaticClass()->GetQualifiedName().ToString(),
			.RendererName = "Texture2DBuiltThumbnail",
			.GeneratorSchemaVersion = 2};
	}

	auto DTextureThumbnailRenderer::CaptureGenerationRequest(
		const ::Durin::Editor::FAssetThumbnailRequest& Request,
		uint64 RendererGeneration) -> std::expected<::Durin::Editor::FAssetThumbnailGenerationRequest, std::string>
	{
		::Durin::Editor::FAssetThumbnailGenerationRequest GenerationRequest;

		if (Request.Asset.AssetClassName != GetRegistration().AssetClassName)
		{
			return std::unexpected("The Texture2D thumbnail renderer received the wrong asset class.");
		}
		const FTopLevelAssetCatalogEntry Entry =
			FindTopLevelAssetExact(Request.Asset.AssetPath);
		if (!Entry || Entry->AssetClassName != Request.Asset.AssetClassName
			|| Entry.Package->FormatVersion != Request.Asset.PackageFormatVersion
			|| static_cast<uint64>(Entry.Package->FileSize) != Request.Asset.FileSize
			|| Entry.Package->LastWriteTimeTicks != Request.Asset.LastWriteTimeTicks)
		{
			return std::unexpected("Texture2D thumbnail registry data is missing or changed.");
		}
		// Capture package identity only. Loading and GPU readiness belong to the
		// scheduled cold-miss session; warm cache hits never inspect source pixels.
		GenerationRequest.KeyInput.Asset = Request.Asset;
		GenerationRequest.KeyInput.RendererName = GetRegistration().RendererName;
		GenerationRequest.KeyInput.GeneratorSchemaVersion = GetRegistration().GeneratorSchemaVersion;
		GenerationRequest.KeyInput.Output = {.Width = 256, .Height = 256};
		GenerationRequest.KeyInput.PreviewFixtureIdentity = "Texture2D.Built.Auto.RGBA";
		GenerationRequest.KeyInput.PreviewFixtureVersion = 1;
		GenerationRequest.KeyInput.ShaderContractVersion = 2;
		GenerationRequest.Input = std::make_shared<FTextureThumbnailGenerationInput>(Request.Asset.AssetPath);
		GenerationRequest.RendererGeneration = RendererGeneration;
		GenerationRequest.RequestSerial = Request.RequestSerial;
		GenerationRequest.bHasTransparency = true;
		return GenerationRequest;
	}

	auto DTextureThumbnailRenderer::CreateGenerationSession(
		const FAssetThumbnailGenerationRequest&, const IAssetThumbnailGenerationInput& Input)
		-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string>
	{
		const auto* Typed = dynamic_cast<const FTextureThumbnailGenerationInput*>(&Input);
		if (!Typed) { return std::unexpected("Invalid Texture2D thumbnail input."); }
		return std::make_unique<FTextureThumbnailSession>(Typed->AssetPath);
	}
} // namespace Durin::Editor::Texture
