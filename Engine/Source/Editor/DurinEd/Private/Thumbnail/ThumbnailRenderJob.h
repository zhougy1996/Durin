#pragma once
#include "CoreMinimal.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Thumbnail/AssetThumbnailTypes.h"
namespace Durin
{
	// Reports the renderer-owned cold-generation state observed by the shared core.
	enum class EThumbnailDrawStatus : uint8
	{
		WaitingForResources,
		ReadyToRender,
		Failed
	};

	// A draw can defer without reserving a scene; diagnostics describe terminal failures.
	struct FThumbnailDrawResult
	{
		EThumbnailDrawStatus State =
			EThumbnailDrawStatus::Failed;

		std::string Diagnostic;
	};

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
