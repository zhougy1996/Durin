#pragma once

#include "CoreMinimal.h"

#include "MaterialEditorAPI.h"
#include "Materials/MaterialExpressionDescription.h"
#include "Materials/MaterialExpressions.h"

namespace Durin::Editor::Material
{
	enum class EMaterialGraphCreationKind { Direct, AssetBound };
	enum class EMaterialGraphPaletteShape { All, InspectOnly, Scalar, ScalarOrVector4, Float2, Adaptive, AdaptiveVector };

	struct FMaterialGraphExpressionRegistration
	{
		const FMaterialExpressionDescription* Description;
		const char* Category;
		EMaterialGraphCreationKind CreationKind;
		EMaterialGraphPaletteShape PaletteShape;
	};

	MATERIALEDITOR_API auto GetMaterialGraphExpressionRegistrations() -> std::span<const FMaterialGraphExpressionRegistration>;
	MATERIALEDITOR_API auto FindMaterialGraphExpressionRegistration(const DClass* Class) -> const FMaterialGraphExpressionRegistration*;
	MATERIALEDITOR_API auto NewRegisteredMaterialExpression(DClass* Class) -> DMaterialExpression*;

	template<typename Expression>
	auto NewRegisteredMaterialExpression() -> Expression*
	{
		return Cast<Expression>(NewRegisteredMaterialExpression(Expression::StaticClass()));
	}
}
