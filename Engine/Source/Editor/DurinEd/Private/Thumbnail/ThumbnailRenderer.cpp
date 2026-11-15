#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
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
		ReleasePreviewScene();
		Super::BeginDestroy();
	}
	auto DThumbnailRenderer::ReleasePreviewScene() -> void
	{
		PreviewScene.reset();
	}
	auto DThumbnailRenderer::GetPreviewScene() -> FThumbnailPreviewScene&
	{
		checkf(IsInGameThread(), "Thumbnail preview scenes belong to the game thread.");
		if (!PreviewScene) PreviewScene = std::make_unique<FThumbnailPreviewScene>();
		return *PreviewScene;
	}

} // namespace Durin
