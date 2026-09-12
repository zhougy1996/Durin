#pragma once
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialTypes.h"
#include <algorithm>
#include <functional>
#include <unordered_map>

// Frozen pre-function template, used only for exact upgrade recognition.
namespace Durin::AssetForge::Builtins::LegacyUpgrade
{
	constexpr auto MakeCanonicalNodeId(uint32 Index) -> FGuid
	{ return {0x4d350001u, 0x7a6b4c21u, 0x91d2e3f4u, Index + 1u}; }
	inline auto MakeLink(const FMaterialProgramNode& Node) -> FMaterialProgramLink
	{ return {.SourceNodeId = Node.Id}; }
	inline auto MakeImportedSurfaceProgram() -> FMaterialProgram
	{
		using Role = MaterialParameters::EMaterialBuiltinParameterRole;
		const auto& BaseIds = MaterialParameters::GetBuiltinParameterIds(Role::BaseColor);
		const auto& NormalIds = MaterialParameters::GetBuiltinParameterIds(Role::Normal);
		const auto& MetallicIds = MaterialParameters::GetBuiltinParameterIds(Role::Metallic);
		const auto& RoughnessIds = MaterialParameters::GetBuiltinParameterIds(Role::Roughness);
		const auto& AmbientOcclusionIds = MaterialParameters::GetBuiltinParameterIds(Role::AmbientOcclusion);
		const auto& EmissiveIds = MaterialParameters::GetBuiltinParameterIds(Role::Emissive);
		const auto& OpacityIds = MaterialParameters::GetBuiltinParameterIds(Role::Opacity);
		const auto& OpacityMaskIds = MaterialParameters::GetBuiltinParameterIds(Role::OpacityMask);
		FMaterialProgram Program;
		std::vector<std::pair<FGuid, FGuid>> UVExpressions;
		Program.Nodes.reserve(MaterialProgramMaxNodeCount);
		auto AddNode = [&](EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType Type,
			std::vector<FMaterialProgramLink> Inputs = {},
			FGuid ParameterId = {},
			FMaterialProgramLiteral Literal = {})
			-> FMaterialProgramNode& {
			FMaterialProgramNode Node;
			Node.Id = MakeCanonicalNodeId(
				static_cast<uint32>(Program.Nodes.size()));
			Node.Opcode = Opcode;
			Node.ResultType = Type;
			Node.Inputs = std::move(Inputs);
			Node.ParameterId = ParameterId;
			Node.Literal = Literal;
			Program.Nodes.push_back(std::move(Node));
			return Program.Nodes.back();
		};
		auto Parameter = [&](FGuid Id, EMaterialProgramValueType Type)
			-> FMaterialProgramNode& {
			return AddNode(EMaterialProgramOpcode::Parameter, Type, {}, Id);
		};
		auto Sample = [&](FGuid TextureId) -> FMaterialProgramNode& {
			auto& Texture = AddNode(
				EMaterialProgramOpcode::TextureParameter,
				EMaterialProgramValueType::Texture2D, {}, TextureId);
			auto& UV = AddNode(
				EMaterialProgramOpcode::Add,
				EMaterialProgramValueType::Float2);
			UVExpressions.emplace_back(UV.Id, TextureId);
			return AddNode(
				EMaterialProgramOpcode::TextureSample2D,
				EMaterialProgramValueType::Float4,
				{MakeLink(Texture), MakeLink(UV)});
		};
		auto Swizzle = [&](FMaterialProgramNode& Source,
			EMaterialProgramValueType Type,
			std::initializer_list<uint8> Mask) -> FMaterialProgramNode& {
			auto& Node = AddNode(
				EMaterialProgramOpcode::Swizzle, Type, {MakeLink(Source)});
			Node.SwizzleLength = static_cast<uint8>(Mask.size());
			std::array<uint8*, 4> Slots{
				&Node.SwizzleX, &Node.SwizzleY,
				&Node.SwizzleZ, &Node.SwizzleW};
			size_t Index = 0;
			for (uint8 Component : Mask) *Slots[Index++] = Component;
			return Node;
		};
		auto Unary = [&](EMaterialProgramOpcode Opcode,
			FMaterialProgramNode& Input) -> FMaterialProgramNode& {
			return AddNode(Opcode, Input.ResultType, {MakeLink(Input)});
		};
		auto Binary = [&](EMaterialProgramOpcode Opcode,
			FMaterialProgramNode& A,
			FMaterialProgramNode& B) -> FMaterialProgramNode& {
			return AddNode(Opcode, A.ResultType, {MakeLink(A), MakeLink(B)});
		};
		auto Constant = [&](EMaterialProgramValueType Type,
			float X, float Y = 0.0f, float Z = 0.0f, float W = 0.0f)
			-> FMaterialProgramNode& {
			return AddNode(EMaterialProgramOpcode::Constant, Type, {}, {},
				{.X = X, .Y = Y, .Z = Z, .W = W});
		};

		auto& BaseParameter = Parameter(
			BaseIds.Value,
			EMaterialProgramValueType::Float3);
		auto& BaseSample = Sample(BaseIds.Texture);
		auto& BaseRgb = Swizzle(
			BaseSample, EMaterialProgramValueType::Float3, {0, 1, 2});
		auto& BaseSaturated = Unary(
			EMaterialProgramOpcode::Saturate, BaseParameter);
		auto& BaseColor = Binary(
			EMaterialProgramOpcode::Multiply, BaseSaturated, BaseRgb);

		auto& NormalParameter = Parameter(
			NormalIds.Value,
			EMaterialProgramValueType::Float3);
		auto& NormalSample = Sample(NormalIds.Texture);
		auto& NormalRg = Swizzle(
			NormalSample, EMaterialProgramValueType::Float2, {0, 1});
		auto& DecodedNormal = AddNode(
			EMaterialProgramOpcode::DecodeNormalRG,
			EMaterialProgramValueType::Float3, {MakeLink(NormalRg)});
		auto& Normal = Binary(
			EMaterialProgramOpcode::BlendNormalsRNM,
			NormalParameter, DecodedNormal);

		auto MakeScalarProduct = [&](FGuid ParameterId, FGuid TextureId,
			uint8 Component) -> FMaterialProgramNode& {
			auto& Value = Parameter(
				ParameterId, EMaterialProgramValueType::Float);
			auto& TextureSample = Sample(TextureId);
			auto& Channel = Swizzle(
				TextureSample, EMaterialProgramValueType::Float, {Component});
			auto& SaturatedValue = Unary(
				EMaterialProgramOpcode::Saturate, Value);
			auto& SaturatedChannel = Unary(
				EMaterialProgramOpcode::Saturate, Channel);
			return Binary(
				EMaterialProgramOpcode::Multiply,
				SaturatedValue, SaturatedChannel);
		};

		auto& Metallic = MakeScalarProduct(
			MetallicIds.Value, MetallicIds.Texture, 2);
		auto& RoughnessProduct = MakeScalarProduct(
			RoughnessIds.Value, RoughnessIds.Texture, 1);
		auto& RoughnessMinimum = Constant(
			EMaterialProgramValueType::Float, 0.045f);
		auto& RoughnessMaximum = Constant(
			EMaterialProgramValueType::Float, 1.0f);
		auto& Roughness = AddNode(
			EMaterialProgramOpcode::Clamp,
			EMaterialProgramValueType::Float,
			{MakeLink(RoughnessProduct), MakeLink(RoughnessMinimum),
				MakeLink(RoughnessMaximum)});
		auto& AmbientOcclusion = MakeScalarProduct(
			AmbientOcclusionIds.Value, AmbientOcclusionIds.Texture, 0);

		auto& EmissiveParameter = Parameter(
			EmissiveIds.Value,
			EMaterialProgramValueType::Float3);
		auto& EmissiveSample = Sample(EmissiveIds.Texture);
		auto& EmissiveRgb = Swizzle(
			EmissiveSample, EMaterialProgramValueType::Float3, {0, 1, 2});
		auto& Zero3 = Constant(
			EMaterialProgramValueType::Float3, 0.0f, 0.0f, 0.0f);
		auto& PositiveEmissive = Binary(
			EMaterialProgramOpcode::Maximum, EmissiveParameter, Zero3);
		auto& PositiveEmissiveSample = Binary(
			EMaterialProgramOpcode::Maximum, EmissiveRgb, Zero3);
		auto& Emissive = Binary(
			EMaterialProgramOpcode::Add,
			PositiveEmissive, PositiveEmissiveSample);

		auto& Opacity = MakeScalarProduct(
			OpacityIds.Value, OpacityIds.Texture, 3);
		auto& OpacityMask = MakeScalarProduct(
			OpacityMaskIds.Value, OpacityMaskIds.Texture, 0);

		Program.Outputs = {
			.BaseColor = MakeLink(BaseColor),
			.Normal = MakeLink(Normal),
			.Metallic = MakeLink(Metallic),
			.Roughness = MakeLink(Roughness),
			.AmbientOcclusion = MakeLink(AmbientOcclusion),
			.Emissive = MakeLink(Emissive),
			.Opacity = MakeLink(Opacity),
			.OpacityMask = MakeLink(OpacityMask)};
		uint32 NextNodeId = static_cast<uint32>(Program.Nodes.size());
		auto AppendNode = [&](EMaterialProgramOpcode Opcode,
			EMaterialProgramValueType Type,
			std::vector<FMaterialProgramLink> Inputs = {},
			FGuid ParameterId = {},
			FMaterialProgramLiteral Literal = {}) -> FMaterialProgramLink {
			FMaterialProgramNode Node;
			Node.Id = MakeCanonicalNodeId(NextNodeId++);
			Node.Opcode = Opcode;
			Node.ResultType = Type;
			Node.Inputs = std::move(Inputs);
			Node.ParameterId = ParameterId;
			Node.Literal = Literal;
			Program.Nodes.push_back(std::move(Node));
			return MakeLink(Program.Nodes.back());
		};
		auto AppendSwizzleScalar = [&](FMaterialProgramLink Source, uint8 Component) {
			const auto Link = AppendNode(EMaterialProgramOpcode::Swizzle,
				EMaterialProgramValueType::Float, {Source});
			auto& Node = Program.Nodes.back();
			Node.SwizzleLength = 1;
			Node.SwizzleX = Component;
			return Link;
		};

		// Complete UV expression slots after assigning the stable surface node identities.
		std::vector<FMaterialProgramNode> SurfaceNodes = std::move(Program.Nodes);
		Program.Nodes.clear();
		Program.Nodes.reserve(MaterialProgramMaxNodeCount);
		NextNodeId = static_cast<uint32>(SurfaceNodes.size());
		for (auto& Node : SurfaceNodes)
		{
			const auto UVExpression = std::ranges::find(UVExpressions, Node.Id,
				&std::pair<FGuid, FGuid>::first);
			if (UVExpression == UVExpressions.end())
			{
				Program.Nodes.push_back(std::move(Node));
				continue;
			}
			const FGuid PreservedId = Node.Id;
			const Role TextureRole = MaterialParameters::FindBuiltinParameterRole(
				UVExpression->second,
				MaterialParameters::EMaterialBuiltinParameterKind::Texture);
			const auto Ids = MaterialParameters::GetBuiltinParameterIds(TextureRole);
			const auto Channel = AppendNode(EMaterialProgramOpcode::Parameter,
				EMaterialProgramValueType::Float, {}, Ids.UVChannel);
			const auto UV = AppendNode(EMaterialProgramOpcode::UVChannel,
				EMaterialProgramValueType::Float2, {Channel});
			const auto Scale = AppendNode(EMaterialProgramOpcode::Parameter,
				EMaterialProgramValueType::Float2, {}, Ids.UVScale);
			const auto Scaled = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float2, {UV, Scale});
			const auto Rotation = AppendNode(EMaterialProgramOpcode::Parameter,
				EMaterialProgramValueType::Float, {}, Ids.UVRotation);
			const auto Sine = AppendNode(EMaterialProgramOpcode::Sine,
				EMaterialProgramValueType::Float, {Rotation});
			const auto Cosine = AppendNode(EMaterialProgramOpcode::Cosine,
				EMaterialProgramValueType::Float, {Rotation});
			const auto X = AppendSwizzleScalar(Scaled, 0);
			const auto Y = AppendSwizzleScalar(Scaled, 1);
			const auto CX = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float, {Cosine, X});
			const auto SY = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float, {Sine, Y});
			const auto SX = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float, {Sine, X});
			const auto CY = AppendNode(EMaterialProgramOpcode::Multiply,
				EMaterialProgramValueType::Float, {Cosine, Y});
			const auto RotatedX = AppendNode(EMaterialProgramOpcode::Subtract,
				EMaterialProgramValueType::Float, {CX, SY});
			const auto RotatedY = AppendNode(EMaterialProgramOpcode::Add,
				EMaterialProgramValueType::Float, {SX, CY});
			const auto Rotated = AppendNode(EMaterialProgramOpcode::MakeFloat2,
				EMaterialProgramValueType::Float2, {RotatedX, RotatedY});
			const auto Offset = AppendNode(EMaterialProgramOpcode::Parameter,
				EMaterialProgramValueType::Float2, {}, Ids.UVOffset);
			FMaterialProgramNode Final;
			Final.Id = PreservedId;
			Final.Opcode = EMaterialProgramOpcode::Add;
			Final.ResultType = EMaterialProgramValueType::Float2;
			Final.Inputs = {Rotated, Offset};
			Program.Nodes.push_back(std::move(Final));
		}
		return Program;
	}


	// Exact result of the pre-8e5b7eb8f StandardSurface resave route.
	inline auto MakeImportedSurfaceAggregateProgram() -> FMaterialProgram
	{
		const auto Template = MakeImportedSurfaceProgram();
		FMaterialProgram Result;
		Result.Nodes.push_back({.Id = MakeCanonicalNodeId(0), .Opcode = EMaterialProgramOpcode::MakeSurface,
			.ResultType = EMaterialProgramValueType::Surface, .DisplayName = "Standard Surface"});
		Result.Outputs.Surface = {.SourceNodeId = Result.Nodes.front().Id};
		std::unordered_map<FGuid, FMaterialProgramLink> Cloned;
		uint32 NextId = 1;
		std::function<FMaterialProgramLink(FMaterialProgramLink)> Clone = [&](FMaterialProgramLink Link) {
			if (const auto It = Cloned.find(Link.SourceNodeId); It != Cloned.end()) return It->second;
			const auto Source = std::ranges::find(Template.Nodes, Link.SourceNodeId, &FMaterialProgramNode::Id);
			auto Node = *Source;
			for (auto& Input : Node.Inputs) Input = Clone(Input);
			Node.Id = FGuid{0x4d494752, 0, 0, NextId++};
			const FMaterialProgramLink NewLink{Node.Id, 0};
			Cloned.emplace(Link.SourceNodeId, NewLink);
			Result.Nodes.push_back(std::move(Node));
			return NewLink;
		};
		std::vector<FMaterialProgramLink> Inputs;
		for (uint32 I = 0; I < 8; ++I)
			Inputs.push_back(Clone(GetMaterialSurfaceOutputLink(Template.Outputs, static_cast<EMaterialSurfaceOutput>(I))));
		Result.Nodes.front().Inputs = std::move(Inputs);
		return Result;
	}
}
