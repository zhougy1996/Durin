#pragma once

#include "CoreMinimal.h"
#include "StaticMeshEditorAPI.h"
#include "Thumbnail/StaticMeshThumbnailTypes.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Shader/ShaderMapPreparation.h"

#include "StaticMeshThumbnailRenderer.gen.h"

namespace Durin::StaticMeshEditor
{
	// Draws loaded StaticMesh assets using a reusable preview scene.
	DCLASS()
	class DStaticMeshThumbnailRenderer final : public DThumbnailRenderer
	{
		GENERATED_BODY()
	public:
		STATICMESHEDITOR_API explicit DStaticMeshThumbnailRenderer(const FObjectInitializer& Initializer);
		STATICMESHEDITOR_API auto CanVisualizeAsset(DObject* Object) const -> bool override;
		STATICMESHEDITOR_API auto Draw(DObject* Object, FThumbnailDrawContext& Context) -> void override;
	private:
		std::optional<FShaderMapPreparation> Shaders;
	};

}
