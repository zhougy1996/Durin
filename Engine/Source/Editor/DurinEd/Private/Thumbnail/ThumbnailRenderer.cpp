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

	auto FThumbnailGenerationData::MakeKeyInput(const FAssetThumbnailPackageFingerprint& Asset,
		const FThumbnailRenderingInfo& Renderer) const -> FAssetThumbnailKeyInput
	{
		return {.Asset = Asset, .RendererName = Renderer.RendererName,
			.GeneratorSchemaVersion = Renderer.GeneratorSchemaVersion,
			.Output = Output, .PreviewFixtureIdentity = PreviewFixtureIdentity,
			.PreviewFixtureVersion = PreviewFixtureVersion, .ShaderContractVersion = ShaderContractVersion,
			.Dependencies = Dependencies};
	}

	auto DThumbnailRenderer::CreateGenerationSession(const IAssetThumbnailGenerationInput&) -> std::unique_ptr<IThumbnailRendererSession>
	{
		return nullptr;
	}
} // namespace Durin
