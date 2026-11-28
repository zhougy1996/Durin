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
	// Reflected descriptions copy immutable creation information from class defaults.
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
		bool bRequiresAsset = false;
		std::optional<EMaterialProgramValueType> PaletteDefault{};
		bool bShowInPalette = true;
		bool bRegisterForEditorCreation = true;
	};

	ENGINE_API auto GetMaterialExpressionDescriptions() -> std::span<const FMaterialExpressionDescription>;
	ENGINE_API auto FindMaterialExpressionDescription(const DClass* Class) -> const FMaterialExpressionDescription*;
}
