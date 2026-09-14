#pragma once
#include "Materials/MaterialExpressions.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin::Editor::Material::GraphEditInternals
{
	MATERIALEDITOR_API auto MakeParameterExpression(const FMaterialParameterDefinition& Definition)
		-> TStrongObjectPtr<DMaterialExpressionParameter>;
}
