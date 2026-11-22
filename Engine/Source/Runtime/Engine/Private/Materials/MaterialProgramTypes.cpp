#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialAttributeDefinitionMap.h"

namespace Durin
{
	auto IsValidMaterialValueSemantics(const FMaterialValueSemantics& Value) -> bool
	{
		const auto StageBits = static_cast<uint8>(Value.Stages);
		if (Value.Type > EMaterialProgramValueType::MaterialAttributes || StageBits == 0
			|| (StageBits & ~static_cast<uint8>(EMaterialEvaluationStage::Both)) != 0)
			return false;
		if (Value.Kind == EMaterialSpatialKind::None)
			return Value.Space == EMaterialCoordinateSpace::None;
		if (Value.Kind == EMaterialSpatialKind::ScreenCoordinate)
			return Value.Type == EMaterialProgramValueType::Float2
				&& Value.Space == EMaterialCoordinateSpace::Screen;
		return Value.Type == EMaterialProgramValueType::Float3
			&& Value.Space >= EMaterialCoordinateSpace::Object
			&& Value.Space <= EMaterialCoordinateSpace::Tangent;
	}

	auto GetMaterialValueSemanticsName(const FMaterialValueSemantics& Value)
		-> std::string
	{
		constexpr std::array Types{"Float", "Float2", "Float3", "Float4",
			"Texture2D", "TextureCube", "MaterialAttributes"};
		constexpr std::array Kinds{"", "Position", "Direction", "Normal",
			"ScreenCoordinate"};
		constexpr std::array Spaces{"", "Object", "World", "View", "Tangent",
			"Screen"};
		const auto Type = static_cast<size_t>(Value.Type);
		const auto Kind = static_cast<size_t>(Value.Kind);
		const auto Space = static_cast<size_t>(Value.Space);
		if (Type >= Types.size() || Kind >= Kinds.size() || Space >= Spaces.size())
			return "Invalid";
		std::string Result = Types[Type];
		if (Value.Kind != EMaterialSpatialKind::None)
			Result += std::format(" {} {}", Spaces[Space], Kinds[Kind]);
		Result += Value.Stages == EMaterialEvaluationStage::Both ? " [Vertex|Pixel]"
			: Value.Stages == EMaterialEvaluationStage::Vertex ? " [Vertex]" : " [Pixel]";
		return Result;
	}

	auto FMaterialAttributeValues::Get(EMaterialProperty Property) -> FMaterialAttributeValue&
	{
		const auto* Definition = FMaterialAttributeDefinitionMap::Find(Property);
		check(Definition);
		for (auto& Value : Bindings) if (Value.AttributeID == Definition->AttributeID) return Value;
		Bindings.reserve(64);
		Bindings.push_back({Definition->AttributeID, {}, Definition->DefaultValue});
		return Bindings.back();
	}
	auto FMaterialAttributeValues::Get(EMaterialProperty Property) const -> const FMaterialAttributeValue&
	{
		const auto ID = FMaterialAttributeDefinitionMap::GetID(Property);
		for (const auto& Value : Bindings) if (Value.AttributeID == ID) return Value;
		static const auto Defaults = [] {
			FMaterialAttributeValues Result;
			for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions()) Result.Get(Definition.Property);
			return Result;
		}();
		for (const auto& Value : Defaults.Bindings) if (Value.AttributeID == ID) return Value;
		check(false);
		return Defaults.Bindings.front();
	}
	auto GetMaterialSurfaceOutputSemantics(EMaterialProperty Output) -> FMaterialValueSemantics
	{
		const auto* Definition = FMaterialAttributeDefinitionMap::Find(Output);
		return Definition ? Definition->Semantics : FMaterialValueSemantics{.Stages=EMaterialEvaluationStage::None};
	}
	auto IsMaterialSurfaceOutputActive(EMaterialProperty Output, const FMaterialStaticProperties& Properties) -> bool
	{
		return std::ranges::contains(FMaterialAttributeDefinitionMap::GetActiveProperties(
			EMaterialDomain::Surface, Properties), Output);
	}
	auto GetMaterialSurfaceOutputType(EMaterialProperty Output) -> EMaterialProgramValueType
	{
		const auto* Definition = FMaterialAttributeDefinitionMap::Find(Output);
		return Definition ? Definition->Semantics.Type : static_cast<EMaterialProgramValueType>(0xff);
	}
	auto GetMaterialSurfaceOutputLink(FMaterialAttributeValues& Outputs, EMaterialProperty Output) -> FMaterialProgramLink&
	{ return Outputs.Get(Output).Connection; }
	auto GetMaterialSurfaceOutputLink(const FMaterialAttributeValues& Outputs, EMaterialProperty Output) -> const FMaterialProgramLink&
	{ return Outputs.Get(Output).Connection; }
	auto GetMaterialSurfaceOutputDefault(FMaterialAttributeValues& Outputs, EMaterialProperty Output) -> FMaterialProgramLiteral&
	{ return Outputs.Get(Output).Literal; }
	auto GetMaterialSurfaceOutputDefault(const FMaterialAttributeValues& Outputs, EMaterialProperty Output) -> const FMaterialProgramLiteral&
	{ return Outputs.Get(Output).Literal; }

	auto SanitizeMaterialGraphPresentation(
		const FMaterialGraphPresentation& Presentation,
		std::span<const FGuid> ExpressionIds) -> FMaterialGraphPresentation
	{
		FMaterialGraphPresentation Result;
		Result.Nodes.reserve(std::min<size_t>(
			Presentation.Nodes.size(), MaterialProgramMaxNodeCount));
		std::unordered_set<FGuid> LiveNodes;
		LiveNodes.reserve(ExpressionIds.size());
		for (const FGuid& Id : ExpressionIds)
			if (Id.IsValid()) LiveNodes.insert(Id);
		std::unordered_set<FGuid> AddedNodes;
		AddedNodes.reserve(Result.Nodes.capacity());
		for (const FMaterialGraphNodePresentation& Node : Presentation.Nodes)
		{
			if (Result.Nodes.size() >= MaterialProgramMaxNodeCount) break;
			if (!Node.NodeId.IsValid() || !LiveNodes.contains(Node.NodeId)
				|| !AddedNodes.insert(Node.NodeId).second
				|| Node.X < -MaterialGraphPresentationCoordinateLimit
				|| Node.X > MaterialGraphPresentationCoordinateLimit
				|| Node.Y < -MaterialGraphPresentationCoordinateLimit
				|| Node.Y > MaterialGraphPresentationCoordinateLimit)
				continue;
			Result.Nodes.push_back(Node);
		}
		std::ranges::sort(Result.Nodes, {},
			&FMaterialGraphNodePresentation::NodeId);
		return Result;
	}

}
