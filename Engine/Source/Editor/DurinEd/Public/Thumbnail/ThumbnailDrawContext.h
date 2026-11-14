#pragma once

#include "CoreMinimal.h"
#include "Thumbnail/ThumbnailRenderTypes.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class DObject;
	class FThumbnailPreviewScene;
	class FAssetThumbnailPool;
	struct FThumbnailManagerTestAccess;

	// One pool-owned draw attempt. Snapshots and scene reservations
	// live here until readback completes; renderers supply no session factory.
	class FThumbnailDrawContext final
	{
	public:
		DURINED_API explicit FThumbnailDrawContext(DObject* Object);
		DURINED_API ~FThumbnailDrawContext();
		FThumbnailDrawContext(const FThumbnailDrawContext&) = delete;
		FThumbnailDrawContext& operator=(const FThumbnailDrawContext&) = delete;
		// A draw starts waiting; only completed setup marks it ready.
		DURINED_API auto MarkReady() -> void;
		DURINED_API auto Fail(std::string Diagnostic) -> void;
		DURINED_API auto SetValidation(std::function<bool()> Validator) -> void;
		// False defers for a busy scene; an unavailable scene fails this draw.
		DURINED_API auto TrySetScene(FThumbnailPreviewScene& Scene) -> bool;
		DURINED_API auto SetView(const FThumbnailPreviewView& View) -> void;
		DURINED_API auto SetViewEnvironment(const FViewEnvironmentOverride& Environment) -> void;
		DURINED_API auto SetImageRenderer(FThumbnailImageRenderer Renderer) -> void;
	private:
		friend class FAssetThumbnailPool;
		friend struct FThumbnailManagerTestAccess;
		enum class EState : uint8 { Waiting, Ready, Failed };
		EState Status = EState::Waiting;
		std::string Diagnostic;
		DURINED_API auto Validate() const -> std::expected<void, std::string>;
		DURINED_API auto BindCapture(class FThumbnailCapture& Capture) -> std::expected<void, std::string>;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
