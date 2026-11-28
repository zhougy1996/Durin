#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"

#include "MaterialEditorAPI.h"
#include "Materials/MaterialExpressionDescription.h"

namespace Durin::MaterialEditor
{
	inline auto CanCreateMaterialExpressionDirectly(const FMaterialExpressionDescription& Description) -> bool
	{
		return Description.bRegisterForEditorCreation && !Description.bRequiresAsset && !Description.Shapes.empty();
	}

	MATERIALEDITOR_API auto NewRegisteredMaterialExpression(DClass* Class) -> DMaterialExpression*;

	template<typename Expression>
	auto NewRegisteredMaterialExpression() -> Expression*
	{
		return Cast<Expression>(NewRegisteredMaterialExpression(Expression::StaticClass()));
	}
}
