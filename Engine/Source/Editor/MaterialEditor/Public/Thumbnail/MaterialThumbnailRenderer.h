#pragma once

#include "CoreMinimal.h"

#include "MaterialEditorAPI.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Shader/ShaderMapPreparation.h"

#include "MaterialThumbnailRenderer.gen.h"

namespace Durin { class FStreamableHandle; }

namespace Durin::MaterialEditor
{
	// Draws materials using a reusable scene and lazily loaded preview sphere.
	DCLASS()
	class DMaterialThumbnailRenderer final : public DThumbnailRenderer
	{
		GENERATED_BODY()
	public:
		MATERIALEDITOR_API explicit DMaterialThumbnailRenderer(const FObjectInitializer& Initializer);

		MATERIALEDITOR_API auto CanVisualizeAsset(DObject* Object) const -> bool override;
		MATERIALEDITOR_API auto Draw(DObject* Object, FThumbnailDrawContext& Context) -> void override;
		MATERIALEDITOR_API auto BeginDestroy() -> void override;
	private:
		std::optional<FShaderMapPreparation> Shaders;
		std::shared_ptr<FStreamableHandle> SphereLoad;

	};
} // namespace Durin::MaterialEditor
