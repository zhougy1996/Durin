#pragma once
#include "CoreMinimal.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Thumbnail/AssetThumbnailTypes.h"
namespace Durin
{
	class FThumbnailRenderJob final
	{
	public:
		DURINED_API FThumbnailRenderJob(DThumbnailRenderer* Renderer,
			FAssetThumbnailPackageFingerprint Asset, DClass* AssetClass = nullptr, DObject* LoadedOverride = nullptr);
		DURINED_API ~FThumbnailRenderJob();
		DURINED_API auto Update(bool bAllowDraw = true) -> FThumbnailDrawResult;
		DURINED_API auto Validate() const -> std::expected<void, std::string>;
		DURINED_API auto BindCapture(class FThumbnailCapture& Capture) -> std::expected<void, std::string>;
	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
