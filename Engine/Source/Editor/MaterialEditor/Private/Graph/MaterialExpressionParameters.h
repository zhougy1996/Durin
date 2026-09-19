#pragma once
#include "MaterialGraphOperations.h"
#include "Materials/MaterialExpressions.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin::Editor::Material::GraphEditInternals
{
	struct FParameterExpressionResult
	{
		TStrongObjectPtr<DMaterialExpressionParameter> Expression;
		std::string Message;
		explicit operator bool() const { return Expression.Get() != nullptr; }
	};
	MATERIALEDITOR_API auto MakeParameterExpression(const FMaterialParameterDefinition& Definition)
		-> FParameterExpressionResult;
	// Resolves a new/rebound node by name, or synchronizes an existing shared definition.
	// Live edits register synchronized peers before writing their shared definition.
	class FGraphEditSession;
	auto ResolveParameterExpression(FGraphEditSession& State, DMaterialExpressionParameter& Parameter,
		const DMaterialExpressionParameter* Previous = nullptr) -> FMaterialGraphCommandResult;
}

namespace Durin::Editor::Material::GraphEditInternals
{
	inline auto MakeParameterMask(const DMaterialExpressionParameter& Parameter, EMaterialProgramValueType Type,
		FGuid Id = FGuid::NewGuid()) -> TStrongObjectPtr<DMaterialExpressionSwizzle>
	{
		TStrongObjectPtr<DMaterialExpressionSwizzle> Mask(NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None));
		Mask->Id = Id; Mask->Input = {Parameter.Id}; Mask->Components.clear();
		for (uint8 Channel = 0; Channel <= static_cast<uint8>(Type); ++Channel) Mask->Components.push_back(Channel);
		return Mask;
	}
}
