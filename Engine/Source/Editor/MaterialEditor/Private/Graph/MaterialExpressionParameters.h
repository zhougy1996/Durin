#pragma once
#include "MaterialEditorAPI.h"
#include "Materials/MaterialExpressions.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin::Editor::Material { struct FMaterialGraphDocumentState; }

namespace Durin::Editor::Material::GraphEditInternals
{
	MATERIALEDITOR_API auto MakeParameterExpression(const FMaterialParameterDefinition& Definition)
		-> TStrongObjectPtr<DMaterialExpressionParameter>;
	// Resolves a new/rebound node by name, or synchronizes an existing shared definition.
	// Candidates only: callers publish the entire state atomically after validation.
	auto ResolveParameterExpression(FMaterialGraphDocumentState& State, DMaterialExpressionParameter& Parameter,
		const DMaterialExpressionParameter* Previous = nullptr) -> std::string;
}
