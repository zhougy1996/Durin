#pragma once

#include "MaterialGraphOperations.h"
#include "Graph/MaterialGraphValueTypes.h"
#include "Materials/Material.h"

namespace Durin::Editor::Material
{
	// Keep canvas, framing and automatic layout on the same node-specific geometry.
	inline auto IsHeaderOnlyGraphNode(const FMaterialGraphNodeView& Node) -> bool
	{
		return Node.Node.Opcode == EMaterialProgramOpcode::Constant
			|| Node.Node.Opcode == EMaterialProgramOpcode::Time
			|| Node.Node.Opcode == EMaterialProgramOpcode::WorldPosition;
	}

	inline auto IsCompactGraphOperation(const FMaterialGraphNodeView& Node) -> bool
	{
		return IsMaterialAdaptiveNumeric(Node.Node.Opcode)
			|| Node.Node.Opcode == EMaterialProgramOpcode::Swizzle;
	}

	inline auto GraphNodeWidth(const FMaterialGraphNodeView& Node) -> float
	{
		if (Node.Node.Opcode == EMaterialProgramOpcode::Constant)
			return 112.0f + 32.0f * static_cast<float>(Node.Node.ResultType);
		if (IsHeaderOnlyGraphNode(Node)) return 160.0f;
		if (Node.Node.Opcode == EMaterialProgramOpcode::Parameter) return 192.0f;
		return IsCompactGraphOperation(Node) ? 160.0f : FMaterialGraphGeometry::GetMetrics().NodeWidth;
	}

	inline auto GraphNodePinOffset(const FMaterialGraphNodeView& Node) -> float
	{
		const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
		if (IsHeaderOnlyGraphNode(Node) || Node.Node.Opcode == EMaterialProgramOpcode::Parameter)
			return Metrics.HeaderHeight * 0.5f;
		return Metrics.HeaderHeight + Metrics.BodyPadding
			+ (IsCompactGraphOperation(Node) ? 0.0f : Metrics.SecondaryHeight);
	}

	inline auto GraphNodeHeight(const FMaterialGraphNodeView& Node) -> float
	{
		const auto& Metrics = FMaterialGraphGeometry::GetMetrics();
		if (IsHeaderOnlyGraphNode(Node)) return Metrics.HeaderHeight;
		if (Node.Node.Opcode == EMaterialProgramOpcode::Parameter)
			return Metrics.HeaderHeight + Metrics.SecondaryHeight;
		const auto Rows = std::max({Node.Inputs.size(), Node.Outputs.size(),
			Node.Node.Opcode == EMaterialProgramOpcode::TextureParameter ? size_t(5) : size_t(1)});
		return GraphNodePinOffset(Node) + Metrics.BodyPadding + Metrics.PinRowHeight * Rows;
	}

	inline auto GraphOutputLabel(const FMaterialGraphNodeView& Node, size_t Index) -> std::string
	{
		const auto& Pin = Node.Outputs[Index];
		return IsHeaderOnlyGraphNode(Node)
			|| (Node.Outputs.size() == 1 && !Pin.PortId.IsValid() && Pin.Name == GetProgramTypeName(Pin.Type))
			? std::string{} : Pin.Name;
	}

	inline auto FormatGraphNumericValue(EMaterialProgramValueType Type,
		const FMaterialProgramLiteral& Value, int Precision = 4) -> std::string
	{
		const std::array Components{Value.X, Value.Y, Value.Z, Value.W};
		if (Type > EMaterialProgramValueType::Float4) return {};
		std::string Text;
		for (size_t Index = 0; Index <= static_cast<size_t>(Type); ++Index)
		{
			if (Index) Text += ", ";
			Text += std::format("{:.{}g}", Components[Index] == 0 ? 0.0f : Components[Index], Precision);
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
