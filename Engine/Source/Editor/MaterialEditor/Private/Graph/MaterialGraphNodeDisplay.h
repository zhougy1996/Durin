#pragma once

#include "MaterialGraphOperations.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "Materials/Material.h"

namespace Durin::Editor::Material
{
	inline auto FormatGraphNumericValue(EMaterialProgramValueType Type,
		const FMaterialProgramLiteral& Value) -> std::string
	{
		const std::array Components{Value.X, Value.Y, Value.Z, Value.W};
		if (Type > EMaterialProgramValueType::Float4) return {};
		std::string Text;
		for (size_t Index = 0; Index <= static_cast<size_t>(Type); ++Index)
		{
			if (Index) Text += ", ";
			Text += std::format("{:.4g}", Components[Index] == 0 ? 0.0f : Components[Index]);
		}
		return Type == EMaterialProgramValueType::Float ? Text : "(" + Text + ")";
	}

	struct FMaterialGraphNodeDisplay
	{
		std::string Title;
		std::string Subtitle;
		std::optional<FMaterialProgramLiteral> Value;
	};

	inline auto MakeGraphNodeDisplay(const FMaterialGraphNodeView& Node,
		const DMaterial* Material = nullptr) -> FMaterialGraphNodeDisplay
	{
		FMaterialGraphNodeDisplay Display{Node.PrimaryLabel, Node.SecondaryLabel};
		if (Node.Node.Opcode == EMaterialProgramOpcode::Constant)
		{
			Display.Value = Node.Node.GetConstantLiteral();
			Display.Title = FormatGraphNumericValue(Node.Node.ResultType, *Display.Value);
			Display.Subtitle = std::format("Constant ({})", GetProgramTypeName(Node.Node.ResultType));
		}
		else if (Node.Node.Opcode == EMaterialProgramOpcode::Parameter && Material)
		{
			FResolvedMaterialParameter Resolved;
			if (Material->ResolveParameterValue(Node.Node.GetParameterId(), Resolved))
			{
				Display.Value = ReadParameterLiteral(Node.Node.ResultType, Resolved.Value);
				Display.Subtitle = FormatGraphNumericValue(Node.Node.ResultType, *Display.Value);
			}
		}
		return Display;
	}
}
