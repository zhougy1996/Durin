#pragma once

#include "CoreMinimal.h"
#include "StaticMeshEditorAPI.h"
#include "Thumbnail/StaticMeshThumbnailTypes.h"

#include "StaticMeshThumbnailRenderer.gen.h"

namespace Durin::StaticMeshEditor
{
	// Captures the exact StaticMesh fingerprint and sorted transitive dependency closure.
	DCLASS()
	class DStaticMeshThumbnailRenderer final : public DThumbnailRenderer
	{
		GENERATED_BODY()
	public:
		STATICMESHEDITOR_API explicit DStaticMeshThumbnailRenderer(const FObjectInitializer& Initializer);
		STATICMESHEDITOR_API auto GetRegistration() const
			-> FThumbnailRenderingInfo override;
		STATICMESHEDITOR_API auto CaptureGenerationInput(const FAssetThumbnailPackageFingerprint& Asset)
			-> std::expected<FThumbnailGenerationData, std::string> override;
		STATICMESHEDITOR_API auto CreateGenerationSession(const IAssetThumbnailGenerationInput& Input)
			-> std::unique_ptr<IThumbnailRendererSession> override;
	};

}
