#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
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

		class FTextureThumbnailDrawState final
		{
		public:
			FTextureThumbnailDrawState(DThumbnailRenderer& InRenderer, DObject* Object, FTopLevelAssetPath Path)
				: AssetPath(std::move(Path))
			{
				(void)InRenderer;
				Texture = Cast<DTexture2D>(Object);
				Shaders = FShaderMapPreparation::Request("DurinEd", {"DurinEd.TexturePreview"}, EShaderBuildPriority::High);
			}

			~FTextureThumbnailDrawState() { ResetPreview(); }

			auto Draw(FThumbnailDrawContext& Context) -> void
			{
				if (!bAssetLoaded)
				{

					if (!Texture.IsValid()) return Context.Fail("Texture2D asset could not be loaded.");
					if (!Texture.Get()->IsAsyncCacheComplete())
						return;
					AssetRevision = Texture.Get()->GetPackage() ? Texture.Get()->GetPackage()->GetEditRevision() : 0;
					Platform = Texture.Get()->GetPlatformDataShared();
					Options.Usage = Texture.Get()->GetUsage();
					bAssetLoaded = true;
				}
				if (!Texture.IsValid()) return Context.Fail("Texture2D asset is unavailable.");
				if (!Texture.Get()->IsAsyncCacheComplete() || Texture.Get()->HasPendingRenderResourceInitialization())
					return;
				const auto* Resource = Texture.Get()->GetResource();
				if (!Resource || !Resource->GetTextureRHI_GameThread())
					return Context.Fail("Texture2D built render resource is unavailable.");
				auto ShaderReady = Shaders.Poll();
				if (!ShaderReady) return Context.Fail(ShaderReady.error());
				if (!*ShaderReady) return;
				Snapshot = Resource ? Resource->GetTextureRHI_GameThread() : FTextureRHIRef{};
				if (!Snapshot) { return Context.Fail("Texture2D built allocation is unavailable."); }
				Context.SetImageRenderer([Input = Snapshot, Display = Options](
					FRHICommandListImmediate& Commands, uint32 Width, uint32 Height) {
					return RenderTexturePreview(Commands, Input, Width, Height, Display, true);
				});
				Context.SetValidation([this] { return IsSnapshotCurrent(); });
				Context.MarkReady();
			}

			auto IsSnapshotCurrent() const -> bool
			{
				if (!Texture.IsValid() || !Texture.Get()->IsAsyncCacheComplete()
					|| Texture.Get()->HasPendingRenderResourceInitialization() || !Snapshot
					|| !Texture.Get()->GetResource()
					|| Texture.Get()->GetResource()->GetTextureRHI_GameThread() != Snapshot
					|| Texture.Get()->GetPlatformDataShared() != Platform || Texture.Get()->GetUsage() != Options.Usage
					|| (Texture.Get()->GetPackage() ? Texture.Get()->GetPackage()->GetEditRevision() : 0) != AssetRevision)
				{
					return false;
				}
				return true;
			}
			auto ResetPreview() -> void
			{
				Snapshot = nullptr;
				Texture = nullptr;
				Platform.reset();
				bAssetLoaded = false;
				Shaders = {};
			}
		private:
			FShaderMapPreparation Shaders;
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

	auto DTextureThumbnailRenderer::DescribeThumbnail(const FAssetThumbnailPackageFingerprint& Asset) -> std::expected<FThumbnailCacheDescription, std::string>
	{
		FThumbnailCacheDescription Result;

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

		Result.bHasTransparency = true;
		return Result;
	}

	auto DTextureThumbnailRenderer::CanVisualizeAsset(DObject* Object) const -> bool
	{
		return Cast<DTexture2D>(Object) != nullptr;
	}

	auto DTextureThumbnailRenderer::Draw(DObject* Object, FThumbnailDrawContext& Context) -> void
	{
		if (!CanVisualizeAsset(Object)) return Context.Fail("The renderer cannot visualize this asset.");
		auto& State = Context.GetState<FTextureThumbnailDrawState>(*this, Object, Context.GetAssetPath());
		State.Draw(Context);
	}

} // namespace Durin::TextureEditor
