#pragma once

#include "CoreMinimal.h"
#include "MaterialEditorAPI.h"
#include "DObject/Object.h"
#include "DObject/StrongObjectPtr.h"
#include "PropertyEditor/PropertyHandle.h"
#include "DObject/WeakObjectPtr.h"
#include "Materials/MaterialTypes.h"
#include "MaterialEditorPreviewParameter.gen.h"

namespace Durin { class DMaterial; }

namespace Durin::MaterialEditor
{
	class FMaterialEditorPreviewParameters;

	DCLASS(NoClassDefaultObject)
	class DMaterialEditorPreviewParameter : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialEditorPreviewParameter(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		float Scalar = 0.0f;
		DPROPERTY()
		FVector4 Vector4{};
		DPROPERTY()
		FMaterialTextureValue Texture;

		MATERIALEDITOR_API auto GetEditTarget() -> FPropertyEditTarget;
		auto CanEditChange(const FProperty* Property) const -> bool override;
		auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		MATERIALEDITOR_API auto GetValue() const -> FMaterialParameterValue;
		auto CopyToSourceInstance(bool bForceStaticPermutationUpdate = false) -> void;
		auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

		FGuid ParameterId;
	private:
		friend class FMaterialEditorPreviewParameters;
		void Initialize(FMaterialEditorPreviewParameters& Owner, const FMaterialParameterDefinition& Definition);
		FMaterialEditorPreviewParameters* Collection = nullptr;
		EMaterialParameterType Type{};
		FName Member;
	};

	// Owns reflected parameter bindings for one material editor document.
	class FMaterialEditorPreviewParameters
	{
	public:
		MATERIALEDITOR_API explicit FMaterialEditorPreviewParameters(DMaterial* Material);
		MATERIALEDITOR_API ~FMaterialEditorPreviewParameters();
		FMaterialEditorPreviewParameters(const FMaterialEditorPreviewParameters&) = delete;
		auto operator=(const FMaterialEditorPreviewParameters&) -> FMaterialEditorPreviewParameters& = delete;
		MATERIALEDITOR_API void RegenerateArrays();
		MATERIALEDITOR_API auto Find(const FGuid& Id) const -> DMaterialEditorPreviewParameter*;
	private:
		friend class DMaterialEditorPreviewParameter;
		void Clear();
		TWeakObjectPtr<DMaterial> Material;
		std::unordered_map<FGuid, TStrongObjectPtr<DMaterialEditorPreviewParameter>> Parameters;
	};
}
