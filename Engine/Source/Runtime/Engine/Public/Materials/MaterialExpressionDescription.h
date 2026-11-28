#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin
{
	class DClass;

	enum class EMaterialExpressionOutputBehavior { Fixed, AdaptiveNumeric, Instance };

	// Authored pins may accept implicit casts or depend on instance state.
	// These shapes are not normalized MIR operation signatures.
	struct FMaterialExpressionAuthoringShape
	{
		EMaterialProgramValueType ResultType;
		std::vector<std::string> InputNames;
		std::vector<std::vector<EMaterialProgramValueType>> AcceptedInputTypes;
	};

	inline auto AppendMaterialValidCastSources(std::vector<EMaterialProgramValueType>& Types) -> void
	{
		if (Types.size() != 1) return;
		const auto DestType = Types.front();
		for (const auto SourceType : {EMaterialProgramValueType::Float, EMaterialProgramValueType::Float2,
			EMaterialProgramValueType::Float3, EMaterialProgramValueType::Float4})
			if (SourceType != DestType && CanValidCast(SourceType, DestType)) Types.push_back(SourceType);
	}

	// Process-lifetime class adapter; all class pointers are borrowed from reflection.
	// No expression instances, asset owners, or editor policy are retained here.
	struct FMaterialExpressionDescription
	{
		DClass* ExpressionClass;
		std::optional<EMaterialProgramOpcode> SemanticOpcode;
		std::string_view Category;
		std::string_view Name;
		std::string_view Description;
		std::string_view SearchKeywords;
		EMaterialExpressionOutputBehavior OutputBehavior;
		std::vector<FMaterialExpressionAuthoringShape> Shapes;
	};

	ENGINE_API auto GetMaterialExpressionDescriptions() -> std::span<const FMaterialExpressionDescription>;
	ENGINE_API auto FindMaterialExpressionDescription(const DClass* Class) -> const FMaterialExpressionDescription*;
}
