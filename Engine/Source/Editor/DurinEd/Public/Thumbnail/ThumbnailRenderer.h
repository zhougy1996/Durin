#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/AssetThumbnailKey.h"
#include "DObject/Object.h"

#include "ThumbnailRenderer.gen.h"

namespace Durin
{
	class DThumbnailManager;
	class DThumbnailRenderer;
	class FThumbnailDrawContext;
	class FThumbnailPreviewScene;

	// Cache policy only. No loaded-object or per-request drawing state is retained here.
	struct FThumbnailCacheDescription
	{
		FAssetThumbnailOutputSettings Output;
		std::string PreviewFixtureIdentity;
		uint32 PreviewFixtureVersion = 0;
		uint32 ShaderContractVersion = 0;
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
		bool bHasTransparency = true;

		DURINED_API auto MakeKeyInput(const FAssetThumbnailPackageFingerprint& Asset,
			const FThumbnailRenderingInfo& Renderer) const -> FAssetThumbnailKeyInput;
	};

	// Renders loaded objects on the game thread. The pool owns loading and
	// pending work; a renderer owns its reusable preview scene.
	DCLASS(Abstract)
	class DThumbnailRenderer : public DObject
	{
		GENERATED_BODY()

	public:
		DURINED_API ~DThumbnailRenderer() override;
		DURINED_API auto BeginDestroy() -> void override;
		// Lazily constructed; never needed for a cache hit or a 2D image renderer.
		DURINED_API auto GetPreviewScene() -> FThumbnailPreviewScene&;

		virtual auto GetRegistration() const -> FThumbnailRenderingInfo = 0;
		virtual auto DescribeThumbnail(const FAssetThumbnailPackageFingerprint& Asset)
			-> std::expected<FThumbnailCacheDescription, std::string> = 0;
		virtual auto CanVisualizeAsset(DObject* Object) const -> bool = 0;
		virtual auto Draw(DObject* Object, FThumbnailDrawContext& Context) -> void = 0;
	protected:
		DURINED_API explicit DThumbnailRenderer(
			const FObjectInitializer& ObjectInitializer);
	private:
		std::unique_ptr<FThumbnailPreviewScene> PreviewScene;
	};

} // namespace Durin
