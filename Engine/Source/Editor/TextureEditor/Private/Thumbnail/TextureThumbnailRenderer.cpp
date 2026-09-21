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
				if (!AssetLoad->GetResult()) return {.Diagnostic = AssetLoad->GetResult().Message};
				if (!bAssetLoaded)
				{
					Texture = Cast<DTexture2D>(AssetLoad->GetLoadedObject());
					if (!Texture.IsValid()) return {.Diagnostic = "Texture2D asset could not be loaded."};
					AssetRevision = Texture.Get()->GetPackage() ? Texture.Get()->GetPackage()->GetEditRevision() : 0;
					Platform = Texture.Get()->GetPlatformDataShared();
					Options.Usage = Texture.Get()->GetUsage();
					bAssetLoaded = true;
				}
				if (!Texture.IsValid()) return {.Diagnostic = "Texture2D asset is unavailable."};
				if (Texture.Get()->IsResourceUpdatePending())
					return {.State = EThumbnailRendererSessionState::WaitingForResources};
				if (!Texture.Get()->HasUsableResource())
					return {.Diagnostic = "Texture2D built render resource is unavailable."};
				return {.State = EThumbnailRendererSessionState::ReadyToRender};
			}
			auto PreparePreview(IThumbnailPreviewScene& Scene, std::string& Error) -> bool override
			{
				Snapshot = Texture.IsValid() ? Texture.Get()->GetPublishedTexture() : FTextureRHIRef{};
				if (!Snapshot) { Error = "Texture2D built allocation is unavailable."; return false; }
				return Scene.SetImageRenderer([Input = Snapshot, Display = Options](
					FRHICommandListImmediate& Commands, uint32 Width, uint32 Height) {
					return RenderTexturePreview(Commands, Input, Width, Height, Display);
				}, Error);
			}
			auto ValidatePreparedInput(
				std::string& Error) const -> bool override
			{
				if (!Texture.IsValid() || Texture.Get()->IsResourceUpdatePending() || !Snapshot
					|| !Texture.Get()->HasUsableResource() || Texture.Get()->GetPublishedTexture() != Snapshot
					|| Texture.Get()->GetPlatformDataShared() != Platform || Texture.Get()->GetUsage() != Options.Usage
					|| (Texture.Get()->GetPackage() ? Texture.Get()->GetPackage()->GetEditRevision() : 0) != AssetRevision)
				{
					Error = "Texture2D changed while its thumbnail was being generated.";
					return false;
				}
				Error.clear();
				return true;
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
		uint64 RendererGeneration,
		::Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		if (Request.Asset.AssetClassName != GetRegistration().AssetClassName)
		{
			OutError = "The Texture2D thumbnail renderer received the wrong asset class.";
			return false;
		}
		const FTopLevelAssetCatalogEntry Entry =
			FindTopLevelAssetExact(Request.Asset.AssetPath);
		if (!Entry || Entry->AssetClassName != Request.Asset.AssetClassName
			|| Entry.Package->FormatVersion != Request.Asset.PackageFormatVersion
			|| static_cast<uint64>(Entry.Package->FileSize) != Request.Asset.FileSize
			|| Entry.Package->LastWriteTimeTicks != Request.Asset.LastWriteTimeTicks)
		{
			OutError = "Texture2D thumbnail registry data is missing or changed.";
			return false;
		}
		// Capture package identity only. Loading and GPU readiness belong to the
		// scheduled cold-miss session; warm cache hits never inspect source pixels.
		OutRequest.KeyInput.Asset = Request.Asset;
		OutRequest.KeyInput.RendererName = GetRegistration().RendererName;
		OutRequest.KeyInput.GeneratorSchemaVersion = GetRegistration().GeneratorSchemaVersion;
		OutRequest.KeyInput.Output = {.Width = 256, .Height = 256};
		OutRequest.KeyInput.PreviewFixtureIdentity = "Texture2D.Built.Auto.RGBA";
		OutRequest.KeyInput.PreviewFixtureVersion = 1;
		OutRequest.KeyInput.ShaderContractVersion = 2;
		OutRequest.Input = std::make_shared<FTextureThumbnailGenerationInput>(Request.Asset.AssetPath);
		OutRequest.RendererGeneration = RendererGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		OutRequest.bHasTransparency = true;
		OutError.clear();
		return true;
	}

	auto DTextureThumbnailRenderer::CreateGenerationSession(
		const FAssetThumbnailGenerationRequest&, const IAssetThumbnailGenerationInput& Input,
		std::string& Error) -> std::unique_ptr<IThumbnailRendererSession>
	{
		const auto* Typed = dynamic_cast<const FTextureThumbnailGenerationInput*>(&Input);
		if (!Typed) { Error = "Invalid Texture2D thumbnail input."; return nullptr; }
		Error.clear();
		return std::make_unique<FTextureThumbnailSession>(Typed->AssetPath);
	}
} // namespace Durin::Editor::Texture
