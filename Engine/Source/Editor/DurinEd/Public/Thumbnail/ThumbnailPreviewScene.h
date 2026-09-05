#pragma once

#include "Math/DurinMath.h"
#include "Thumbnail/AssetThumbnailTypes.h"
#include "Thumbnail/ThumbnailRenderer.h"

namespace Durin::Editor
{
	enum class EThumbnailCaptureState : uint8
	{
		Idle,
		Rendering,
		Ready,
		Failed
	};

	// Owns the single resettable preview scene allowed by the initial rendered-thumbnail budget.
	class FThumbnailPreviewScenePool final
		: public IThumbnailPreviewScene
	{
	public:
		DURINED_API explicit FThumbnailPreviewScenePool(
			FAssetThumbnailOutputSettings Output = {},
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API explicit FThumbnailPreviewScenePool(
			FThumbnailVisualContract VisualContract,
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FThumbnailPreviewScenePool();

		FThumbnailPreviewScenePool(const FThumbnailPreviewScenePool&) = delete;
		FThumbnailPreviewScenePool& operator=(const FThumbnailPreviewScenePool&) = delete;

		DURINED_API auto IsAvailable() const -> bool;
		DURINED_API auto GetDiagnostic() const -> std::string;
		DURINED_API auto GetWorld() -> DWorld* override;
		DURINED_API auto SetView(
			const FThumbnailPreviewView& View,
			std::string& OutError) -> bool override;
		DURINED_API auto SetViewEnvironment(
			const FViewEnvironmentOverride& Environment,
			std::string& OutError) -> bool override;
		// Enqueues one render and one readback on the rendering thread. Transparent
		// captures clear to transparent black so UI compositing has no color fringe.
		DURINED_API auto BeginCapture(std::string& OutError) -> bool;
		// Moves completed tightly-packed SRGBA8 pixels to the game thread.
		DURINED_API auto PollCapture(
			FByteBuffer& OutPixels,
			std::string& OutError) -> EThumbnailCaptureState;
		DURINED_API auto Reset() -> void;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

} // namespace Durin::Editor
