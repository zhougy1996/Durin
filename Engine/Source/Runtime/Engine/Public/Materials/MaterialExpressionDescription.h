#pragma once

#include "EngineAPI.h"
#include "Materials/MaterialProgramTypes.h"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Durin
{
	class DClass;

	enum class EMaterialExpressionOutputBehavior { Fixed, AdaptiveNumeric, Instance };

	// Authored pins may accept scalar broadcast or depend on instance state.
	// These shapes are not normalized MIR operation signatures.
	struct FMaterialExpressionAuthoringShape
	{
		EMaterialProgramValueType ResultType;
		std::vector<std::string> InputNames;
		std::vector<std::vector<EMaterialProgramValueType>> AcceptedInputTypes;
	};

	// Process-lifetime class adapter; all class pointers are borrowed from reflection.
	// No expression instances, asset owners, or editor policy are retained here.
	struct FMaterialExpressionDescription
	{
		DClass* ExpressionClass;
		std::optional<EMaterialProgramOpcode> SemanticOpcode;
		std::string_view Name;
		std::string_view Description;
		std::string_view SearchKeywords;
		EMaterialExpressionOutputBehavior OutputBehavior;
		std::vector<FMaterialExpressionAuthoringShape> Shapes;
	};

	ENGINE_API auto GetMaterialExpressionDescriptions() -> std::span<const FMaterialExpressionDescription>;
	ENGINE_API auto FindMaterialExpressionDescription(const DClass* Class) -> const FMaterialExpressionDescription*;
}
