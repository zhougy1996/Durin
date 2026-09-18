#pragma once

#include "TextureEditorAPI.h"
#include "DObject/ObjectValidation.h"
#include "Texture/Texture2DCompilationTypes.h"

namespace Durin::Editor::Texture
{
	enum class ETexture2DPropertyEditError : uint8 { Metadata, Settings, Source, Compilation, DeferredValidation };
	class FTexture2DPropertyEditCause final : public IObjectValidationCause
	{
	public:
		ETexture2DPropertyEditError Code = ETexture2DPropertyEditError::Metadata;
		uint64 ActualKind = 0;
		uint64 ExpectedKind = 0;
		bool HasPackage = false;
		bool HasSource = false;
		std::optional<FTexture2DInputError> InputCause;
		std::optional<FTexture2DCompilationError> CompilationCause;
		TEXTUREEDITOR_API auto Format() const -> std::string override;
	};

	// Installs the Texture2D build-setting adapter into DurinEd's reflected
	// property transaction pipeline. Repeated registration is idempotent.
	TEXTUREEDITOR_API auto RegisterTexture2DPropertyEditing() -> bool;
	// Removes the adapter when present and prevents future property callbacks.
	TEXTUREEDITOR_API auto UnregisterTexture2DPropertyEditing() -> void;
}
