#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	DThumbnailRenderer::DThumbnailRenderer(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	DThumbnailRenderer::~DThumbnailRenderer() = default;
	auto DThumbnailRenderer::BeginDestroy() -> void
	{
		PreviewScene.reset();
		Super::BeginDestroy();
	}
	auto DThumbnailRenderer::GetPreviewScene() -> FThumbnailPreviewScene&
	{
		checkf(IsInGameThread(), "Thumbnail preview scenes belong to the game thread.");
		if (!PreviewScene) PreviewScene = std::make_unique<FThumbnailPreviewScene>();
		return *PreviewScene;
	}

	auto DThumbnailRenderer::CreateGenerationSession(
		const FAssetThumbnailGenerationRequest&,
		const IAssetThumbnailGenerationInput&) -> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string>
	{
		return std::unexpected("The thumbnail renderer produces no preview-scene session.");
	}
} // namespace Durin
