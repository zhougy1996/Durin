#pragma once

#include "CoreMinimal.h"
#include "Thumbnail/ThumbnailDrawContext.h"

namespace Durin
{
	enum class EThumbnailCaptureState : uint8
	{
		Idle,
		Preparing,
		Rendering,
		ReadbackPending,
		Ready,
		Failed
	};

	// Pool-owned output and asynchronous readback. Borrows a renderer scene until Reset.
	class FThumbnailCapture final
	{
	public:
		DURINED_API explicit FThumbnailCapture(FAssetThumbnailOutputSettings Output = {});
		DURINED_API ~FThumbnailCapture();
		FThumbnailCapture(const FThumbnailCapture&) = delete;
		FThumbnailCapture& operator=(const FThumbnailCapture&) = delete;
		DURINED_API auto IsAvailable() const -> bool;
		DURINED_API auto GetDiagnostic() const -> std::string;
		DURINED_API auto SetScene(FThumbnailPreviewScene& Scene) -> std::expected<void, std::string>;
		DURINED_API auto SetView(const FThumbnailPreviewView& View) -> void;
		DURINED_API auto SetViewEnvironment(const FViewEnvironmentOverride& Environment) -> void;
		DURINED_API auto SetImageRenderer(FThumbnailImageRenderer Renderer) -> void;
		DURINED_API auto BeginCapture() -> std::expected<void, std::string>;
		// Empty while pending; completed tightly packed SRGBA8 pixels are consumed once.
		DURINED_API auto PollCapture() -> std::expected<std::optional<FByteBuffer>, std::string>;
		DURINED_API auto Reset() -> void;
	private:
		friend class FThumbnailDrawContext;
		auto BindDrawScene(FThumbnailPreviewScene& Scene) -> void;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
