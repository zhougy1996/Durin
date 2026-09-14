#pragma once
#include "AssetForgeBuiltinsAPI.h"
#include "DObject/StrongObjectPtr.h"
#include "Materials/Material.h"

namespace Durin::AssetForge::Builtins
{
	// Owning-thread authoring result. Publication duplicates concrete children.
	struct FMaterialExpressionRecipe
	{
		std::vector<TStrongObjectPtr<DMaterialExpression>> Expressions;
		FMaterialExpressionSurfaceOutputs Outputs;
		FMaterialGraphPresentation Presentation;
		ASSETFORGEBUILTINS_API auto Apply(DMaterial& Material) const -> FMaterialProgramValidationResult;
		// Compare authored graph fields, excluding independently editable presentation.
		ASSETFORGEBUILTINS_API auto MatchesGraph(const DMaterial& Material) const -> bool;
		ASSETFORGEBUILTINS_API auto MatchesGraph(const FMaterialExpressionRecipe& Other) const -> bool;
	};
}
