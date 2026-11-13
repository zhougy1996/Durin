#pragma once

#include "CoreMinimal.h"
#include "Thumbnail/AssetThumbnailTypes.h"

namespace Durin
{
	class DWorld;
	class AActor;
	class FSceneInterface;

	// Renderer-owned native scene. Fixtures persist; sessions detach asset bindings.
	class FThumbnailPreviewScene final
	{
	public:
		DURINED_API explicit FThumbnailPreviewScene(FThumbnailVisualContract Contract = {});
		DURINED_API ~FThumbnailPreviewScene();
		FThumbnailPreviewScene(const FThumbnailPreviewScene&) = delete;
		FThumbnailPreviewScene& operator=(const FThumbnailPreviewScene&) = delete;
		DURINED_API auto IsAvailable() const -> bool;
		DURINED_API auto GetDiagnostic() const -> std::string;
		DURINED_API auto GetWorld() -> DWorld*;
		DURINED_API auto GetPreviewActor() -> AActor*;
	private:
		friend class FThumbnailCapture;
		friend class FThumbnailDrawContext;
		DURINED_API auto GetRenderScene() -> FSceneInterface*;
		const void* BindingOwner = nullptr;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
