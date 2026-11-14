#include "Thumbnail/TextureThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Texture/Texture2D.h"
#include "Preview/TexturePreview.h"
#include "DObject/WeakObjectPtr.h"
#include "RenderResource.h"

namespace Durin::TextureEditor
{
	DTextureThumbnailRenderer::DTextureThumbnailRenderer(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DTextureThumbnailRenderer::CanVisualizeAsset(DObject* Object) const -> bool { return Cast<DTexture2D>(Object) != nullptr; }
	auto DTextureThumbnailRenderer::Draw(DObject* Object, FThumbnailDrawContext& Context) -> void
	{
		auto* Texture = Cast<DTexture2D>(Object);
		if (!Texture) return Context.Fail("The renderer cannot visualize this asset.");
		if (!Shaders) Shaders = FShaderMapPreparation::Request("DurinEd", {"DurinEd.TexturePreview"}, EShaderBuildPriority::High);
		if (!Texture->IsAsyncCacheComplete() || Texture->HasPendingRenderResourceInitialization()) return;
		const auto* Resource = Texture->GetResource();
		if (!Resource || !Resource->GetTextureRHI_GameThread()) return Context.Fail("Texture2D built render resource is unavailable.");
		auto Ready = Shaders->Poll();
		if (!Ready) return Context.Fail(Ready.error());
		if (!*Ready) return;
		auto Snapshot = Resource->GetTextureRHI_GameThread();
		auto Platform = Texture->GetPlatformDataShared();
		FTexturePreviewOptions Options;
		Options.Usage = Texture->GetUsage();
		Context.SetImageRenderer([Snapshot, Options](FRHICommandListImmediate& Commands, uint32 Width, uint32 Height) {
			return RenderTexturePreview(Commands, Snapshot, Width, Height, Options, true);
		});
		Context.SetValidation([Texture = TWeakObjectPtr<DTexture2D>(Texture), Snapshot, Platform, Usage = Options.Usage] {
			const auto* Current = Texture.Get();
			return Current && Current->IsAsyncCacheComplete() && !Current->HasPendingRenderResourceInitialization()
				&& Current->GetResource() && Current->GetResource()->GetTextureRHI_GameThread() == Snapshot
				&& Current->GetPlatformDataShared() == Platform && Current->GetUsage() == Usage;
		});
		Context.MarkReady();
	}
} // namespace Durin::TextureEditor
