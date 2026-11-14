#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/ThumbnailRenderer.h"
#include "Shader/ShaderMapPreparation.h"
#include "TextureEditorAPI.h"

#include "TextureCubeThumbnailRenderer.gen.h"

namespace Durin::TextureEditor
{
	// Freezes TextureEditor-owned presentation values for the wide environment view.
	struct FTextureCubeThumbnailRendererVisualContract
	{
		static constexpr float VerticalFieldOfViewDegrees = 100.0f;
	};

	// Draws loaded TextureCube assets with the wide environment view.
	DCLASS()
	class DTextureCubeThumbnailRenderer final : public DThumbnailRenderer
	{
		GENERATED_BODY()
	public:
		TEXTUREEDITOR_API explicit DTextureCubeThumbnailRenderer(const FObjectInitializer& Initializer);
		TEXTUREEDITOR_API auto CanVisualizeAsset(DObject* Object) const -> bool override;
		TEXTUREEDITOR_API auto Draw(DObject* Object, FThumbnailDrawContext& Context) -> void override;
	private:
		std::optional<FShaderMapPreparation> Shaders;
	};
} // namespace Durin::TextureEditor
