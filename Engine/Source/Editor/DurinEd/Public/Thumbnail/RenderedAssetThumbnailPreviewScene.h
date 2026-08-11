#pragma once

#include "Math/DurinMath.h"
#include "Thumbnail/AssetThumbnailTypes.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"

namespace Durin::Editor
{
	enum class ERenderedAssetThumbnailCaptureState : uint8
	{
		Idle,
		Rendering,
		Ready,
		Failed
	};

	// Owns the single resettable preview scene allowed by the initial rendered-thumbnail budget.
	class FRenderedAssetThumbnailPreviewScenePool final
		: public IRenderedAssetThumbnailPreviewScene
	{
	public:
		DURINED_API explicit FRenderedAssetThumbnailPreviewScenePool(
			FAssetThumbnailOutputSettings Output = {},
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API explicit FRenderedAssetThumbnailPreviewScenePool(
			FRenderedAssetThumbnailVisualContract VisualContract,
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FRenderedAssetThumbnailPreviewScenePool();

		FRenderedAssetThumbnailPreviewScenePool(const FRenderedAssetThumbnailPreviewScenePool&) = delete;
		FRenderedAssetThumbnailPreviewScenePool& operator=(const FRenderedAssetThumbnailPreviewScenePool&) = delete;

		DURINED_API auto IsAvailable() const -> bool;
		DURINED_API auto GetDiagnostic() const -> std::string;
		DURINED_API auto GetWorld() -> DWorld* override;
		DURINED_API auto SetView(
			const FRenderedAssetThumbnailPreviewView& View,
			std::string& OutError) -> bool override;
		DURINED_API auto SetViewEnvironment(
			const FViewEnvironmentOverride& Environment,
			std::string& OutError) -> bool override;
		// Enqueues one render and one readback on the rendering thread. Transparent
		// captures clear to transparent black so UI compositing has no color fringe.
		DURINED_API auto BeginCapture(std::string& OutError) -> bool;
		// Moves completed tightly-packed SRGBA8 pixels to the game thread.
		DURINED_API auto PollCapture(
			std::vector<uint8>& OutPixels,
			std::string& OutError) -> ERenderedAssetThumbnailCaptureState;
		DURINED_API auto Reset() -> void;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

} // namespace Durin::Editor
