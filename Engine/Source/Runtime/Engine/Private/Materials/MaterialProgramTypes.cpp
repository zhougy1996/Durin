#include "Materials/MaterialProgramTypes.h"

#include "Materials/MaterialTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numeric>
#include <ranges>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	auto GetMaterialSurfaceParameterId(
		EMaterialSurfaceOutput Output,
		MaterialParameters::EMaterialBuiltinParameterKind Kind) -> FGuid
	{
		using MaterialParameters::EMaterialBuiltinParameterRole;
		EMaterialBuiltinParameterRole Role;
		switch (Output)
		{
		case EMaterialSurfaceOutput::BaseColor:
			Role = EMaterialBuiltinParameterRole::BaseColor; break;
		case EMaterialSurfaceOutput::Normal:
			Role = EMaterialBuiltinParameterRole::Normal; break;
		case EMaterialSurfaceOutput::Metallic:
			Role = EMaterialBuiltinParameterRole::Metallic; break;
		case EMaterialSurfaceOutput::Roughness:
			Role = EMaterialBuiltinParameterRole::Roughness; break;
		case EMaterialSurfaceOutput::AmbientOcclusion:
			Role = EMaterialBuiltinParameterRole::AmbientOcclusion; break;
		case EMaterialSurfaceOutput::Emissive:
			Role = EMaterialBuiltinParameterRole::Emissive; break;
		case EMaterialSurfaceOutput::Opacity:
			Role = EMaterialBuiltinParameterRole::Opacity; break;
		case EMaterialSurfaceOutput::OpacityMask:
			Role = EMaterialBuiltinParameterRole::OpacityMask; break;
		default:
			return {};
		}
		return MaterialParameters::GetBuiltinParameterId(Role, Kind);
	}

	namespace
	{
		constexpr auto MakeCanonicalNodeId(uint32 Index) -> FGuid
		{
			return {0x4d350001u, 0x7a6b4c21u, 0x91d2e3f4u, Index + 1u};
		}

		auto MakeLink(const FMaterialProgramNode& Node)
			-> FMaterialProgramLink
		{
			return {.SourceNodeId = Node.Id, .SourceOutputIndex = 0};
		}

		auto GetComponentCount(EMaterialProgramValueType Type) -> uint8
		{
			switch (Type)
			{
			case EMaterialProgramValueType::Float: return 1;
			case EMaterialProgramValueType::Float2: return 2;
			case EMaterialProgramValueType::Float3: return 3;
			case EMaterialProgramValueType::Float4: return 4;
			case EMaterialProgramValueType::Texture2D: return 0;
			case EMaterialProgramValueType::Surface: return 0;
			}
			return 0;
		}

		auto IsValidValueType(EMaterialProgramValueType Type) -> bool
		{
			return GetComponentCount(Type) != 0
				|| Type == EMaterialProgramValueType::Texture2D
				|| Type == EMaterialProgramValueType::Surface;
		}

		auto IsValidOpcode(EMaterialProgramOpcode Opcode) -> bool
		{
			return Opcode >= EMaterialProgramOpcode::Constant
				&& Opcode <= EMaterialProgramOpcode::MakeSurface
				&& static_cast<uint8>(Opcode) != 3 && static_cast<uint8>(Opcode) != 30;
		}

		auto FindParameter(
			std::span<const FMaterialParameterDefinition> Definitions,
			const FGuid& Id) -> const FMaterialParameterDefinition*
		{
			const auto It = std::ranges::find(Definitions, Id,
				&FMaterialParameterDefinition::Id);
			return It == Definitions.end() ? nullptr : &*It;
		}

		auto GetProgramType(EMaterialParameterType Type)
			-> EMaterialProgramValueType
		{
			switch (Type)
			{
			case EMaterialParameterType::Scalar:
				return EMaterialProgramValueType::Float;
			case EMaterialParameterType::Vector2:
				return EMaterialProgramValueType::Float2;
			case EMaterialParameterType::Vector:
				return EMaterialProgramValueType::Float3;
			case EMaterialParameterType::Vector4:
				return EMaterialProgramValueType::Float4;
			case EMaterialParameterType::Texture:
				return EMaterialProgramValueType::Texture2D;
			}
			return static_cast<EMaterialProgramValueType>(0xff);
		}

		auto AddDiagnostic(
			std::vector<FMaterialProgramDiagnostic>& Diagnostics,
			EMaterialProgramDiagnosticCategory Category,
			EMaterialProgramDiagnosticLocationKind LocationKind,
			const FGuid& NodeId,
			uint32 LocationIndex,
			std::string Message) -> void
		{
			if (Diagnostics.size() >= MaterialProgramMaxDiagnosticCount)
				return;
			if (Message.size() > MaterialProgramMaxDiagnosticMessageBytes)
				Message.resize(MaterialProgramMaxDiagnosticMessageBytes);
			Diagnostics.push_back({
				.Category = Category,
				.LocationKind = LocationKind,
				.NodeId = NodeId,
				.LocationIndex = LocationIndex,
				.Message = std::move(Message)});
		}

		auto SortAndBoundDiagnostics(
			std::vector<FMaterialProgramDiagnostic>& Diagnostics) -> void
		{
			std::ranges::sort(Diagnostics, [](const auto& A, const auto& B) {
				return std::tie(
					A.Category, A.NodeId, A.LocationKind,
					A.LocationIndex, A.Message)
					< std::tie(
					B.Category, B.NodeId, B.LocationKind,
					B.LocationIndex, B.Message);
			});
			Diagnostics.erase(
				std::unique(Diagnostics.begin(), Diagnostics.end()),
				Diagnostics.end());
			if (Diagnostics.size() > MaterialProgramMaxDiagnosticCount)
				Diagnostics.resize(MaterialProgramMaxDiagnosticCount);
		}

		auto ValidateNodeShape(
			const FMaterialProgramNode& Node,
			const std::function<const FMaterialProgramNode*(size_t)>& GetInput,
			std::span<const FMaterialParameterDefinition> Definitions,
			std::unordered_set<FGuid>& ReferencedParameters,
			std::vector<FMaterialProgramDiagnostic>& Diagnostics) -> void
		{
			const auto AddType = [&](uint32 Input, std::string Message) {
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Type,
					EMaterialProgramDiagnosticLocationKind::Input,
					Node.Id, Input, std::move(Message));
			};
			const auto Signature = GetMaterialProgramNodeSignature(Node.Opcode, Node.ResultType);
			if (!Signature)
			{
				AddType(0, "Material program opcode does not support its result type.");
				return;
			}
			if (Node.Inputs.size() != Signature->InputCount)
				AddType(0, "Material program opcode has an invalid input count.");
			else
				for (uint32 Index = 0; Index < Signature->InputCount; ++Index)
				{
					const FMaterialProgramNode* Input = GetInput(Index);
					const auto Accepted = Signature->Inputs[Index];
					if (!Input || std::ranges::find(Accepted, Input->ResultType) == Accepted.end())
						AddType(Index, "Material program input type does not match its opcode signature.");
				}
			const auto InputType = [&](size_t Index) {
				const FMaterialProgramNode* Input = GetInput(Index);
				return Input ? Input->ResultType
					: static_cast<EMaterialProgramValueType>(0xff);
			};

			switch (Node.Opcode)
			{
			case EMaterialProgramOpcode::Constant:
			{
				const std::array Values{
					Node.Literal.X, Node.Literal.Y,
					Node.Literal.Z, Node.Literal.W};
				for (uint8 Index = 0; Index < GetComponentCount(Node.ResultType); ++Index)
					if (!std::isfinite(Values[Index]))
						AddType(Index, "Material program constant components must be finite binary32 values.");
				break;
			}
			case EMaterialProgramOpcode::Parameter:
			case EMaterialProgramOpcode::TextureParameter:
			{
				const FMaterialParameterDefinition* Definition =
					FindParameter(Definitions, Node.ParameterId);
				if (!Node.ParameterId.IsValid() || Definition == nullptr)
				{
					AddType(0, "Material program parameter reference is missing or unknown.");
					break;
				}
				ReferencedParameters.insert(Node.ParameterId);
				{
					const EMaterialProgramValueType Expected = GetProgramType(Definition->Type);
					const bool bTextureOpcode = Node.Opcode
						== EMaterialProgramOpcode::TextureParameter;
					if (Node.ResultType != Expected
						|| bTextureOpcode != (Expected
							== EMaterialProgramValueType::Texture2D))
						AddType(0, "Material program parameter opcode or result type does not match the referenced definition.");
				}
				break;
			}
			case EMaterialProgramOpcode::Swizzle:
				if (Node.Inputs.size() == 1)
				{
					const uint8 SourceWidth = GetComponentCount(InputType(0));
					const std::array Mask{
						Node.SwizzleX, Node.SwizzleY,
						Node.SwizzleZ, Node.SwizzleW};
					if (Node.SwizzleLength == 0 || Node.SwizzleLength > 4
						|| GetComponentCount(Node.ResultType) != Node.SwizzleLength)
						AddType(0, "Swizzle length must match its numeric result width.");
					for (uint8 Index = 0; Index < Node.SwizzleLength && Index < 4; ++Index)
						if (Mask[Index] >= SourceWidth)
							AddType(Index, "Swizzle component exceeds the source width.");
				}
				break;
			default:
				break;
			}
		}
	}

	auto MakeDefaultMaterialProgram() -> FMaterialProgram
	{
		return {};
	}

	auto MakePBRMaterialProgram() -> FMaterialProgram
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

	auto GetMaterialSurfaceOutputType(EMaterialSurfaceOutput Output)
		-> EMaterialProgramValueType
	{
		switch (Output)
		{
		case EMaterialSurfaceOutput::BaseColor:
		case EMaterialSurfaceOutput::Normal:
		case EMaterialSurfaceOutput::Emissive:
			return EMaterialProgramValueType::Float3;
		case EMaterialSurfaceOutput::Metallic:
		case EMaterialSurfaceOutput::Roughness:
		case EMaterialSurfaceOutput::AmbientOcclusion:
		case EMaterialSurfaceOutput::Opacity:
		case EMaterialSurfaceOutput::OpacityMask:
			return EMaterialProgramValueType::Float;
		}
		return static_cast<EMaterialProgramValueType>(0xff);
	}

	auto GetMaterialSurfaceOutputLink(
		FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> FMaterialProgramLink&
	{
		return const_cast<FMaterialProgramLink&>(GetMaterialSurfaceOutputLink(
			std::as_const(Outputs), Output));
	}

	auto GetMaterialSurfaceOutputLink(
		const FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> const FMaterialProgramLink&
	{
		switch (Output)
		{
		case EMaterialSurfaceOutput::BaseColor: return Outputs.BaseColor;
		case EMaterialSurfaceOutput::Normal: return Outputs.Normal;
		case EMaterialSurfaceOutput::Metallic: return Outputs.Metallic;
		case EMaterialSurfaceOutput::Roughness: return Outputs.Roughness;
		case EMaterialSurfaceOutput::AmbientOcclusion: return Outputs.AmbientOcclusion;
		case EMaterialSurfaceOutput::Emissive: return Outputs.Emissive;
		case EMaterialSurfaceOutput::Opacity: return Outputs.Opacity;
		case EMaterialSurfaceOutput::OpacityMask: return Outputs.OpacityMask;
		}
		return Outputs.BaseColor;
	}

	auto GetMaterialSurfaceOutputDefault(
		FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> FMaterialProgramLiteral&
	{
		return const_cast<FMaterialProgramLiteral&>(GetMaterialSurfaceOutputDefault(
			std::as_const(Outputs), Output));
	}

	auto GetMaterialSurfaceOutputDefault(
		const FMaterialSurfaceOutputs& Outputs, EMaterialSurfaceOutput Output)
		-> const FMaterialProgramLiteral&
	{
		switch (Output)
		{
		case EMaterialSurfaceOutput::BaseColor: return Outputs.BaseColorDefault;
		case EMaterialSurfaceOutput::Normal: return Outputs.NormalDefault;
		case EMaterialSurfaceOutput::Metallic: return Outputs.MetallicDefault;
		case EMaterialSurfaceOutput::Roughness: return Outputs.RoughnessDefault;
		case EMaterialSurfaceOutput::AmbientOcclusion: return Outputs.AmbientOcclusionDefault;
		case EMaterialSurfaceOutput::Emissive: return Outputs.EmissiveDefault;
		case EMaterialSurfaceOutput::Opacity: return Outputs.OpacityDefault;
		case EMaterialSurfaceOutput::OpacityMask: return Outputs.OpacityMaskDefault;
		}
		return Outputs.BaseColorDefault;
	}

	auto ValidateMaterialProgram(
		const FMaterialProgram& Program,
		std::span<const FMaterialParameterDefinition> ParameterDefinitions)
		-> FMaterialProgramValidationResult
	{
		FMaterialProgramValidationResult Result;
		auto& Diagnostics = Result.Diagnostics;
		Diagnostics.reserve(MaterialProgramMaxDiagnosticCount);
		if (Program.SchemaVersion != CurrentMaterialProgramSchemaVersion)
			AddDiagnostic(Diagnostics,
				EMaterialProgramDiagnosticCategory::Schema,
				EMaterialProgramDiagnosticLocationKind::Program, {}, 0,
				"Material program schema version is unsupported.");
		if (Program.Nodes.size() > MaterialProgramMaxNodeCount)
		{
			AddDiagnostic(Diagnostics,
				EMaterialProgramDiagnosticCategory::Bounds,
				EMaterialProgramDiagnosticLocationKind::Program, {}, 0,
				"Material program node count exceeds the schema bound.");
			SortAndBoundDiagnostics(Diagnostics);
			return Result;
		}

		uint64 LinkCount = Program.Outputs.Surface.SourceNodeId.IsValid() ? 1u : 0u;
		LinkCount += static_cast<uint64>(std::ranges::count_if(
			std::array{
				Program.Outputs.BaseColor.SourceNodeId,
				Program.Outputs.Normal.SourceNodeId,
				Program.Outputs.Metallic.SourceNodeId,
				Program.Outputs.Roughness.SourceNodeId,
				Program.Outputs.AmbientOcclusion.SourceNodeId,
				Program.Outputs.Emissive.SourceNodeId,
				Program.Outputs.Opacity.SourceNodeId,
				Program.Outputs.OpacityMask.SourceNodeId},
			[](const FGuid& Id) { return Id.IsValid(); }));
		uint64 StringBytes = 0;
		uint64 EstimatedBytes = sizeof(Program.SchemaVersion)
			+ 9 * sizeof(FMaterialProgramLink)
			+ 8 * sizeof(FMaterialProgramLiteral);
		std::unordered_map<FGuid, size_t> NodeIndices;
		NodeIndices.reserve(std::min<size_t>(
			Program.Nodes.size(), MaterialProgramMaxNodeCount));
		std::vector<size_t> ScanIndices(Program.Nodes.size());
		std::iota(ScanIndices.begin(), ScanIndices.end(), size_t{0});
		std::ranges::sort(ScanIndices, [&](size_t A, size_t B) {
			return Program.Nodes[A].Id < Program.Nodes[B].Id;
		});
		for (size_t Index : ScanIndices)
		{
			const FMaterialProgramNode& Node = Program.Nodes[Index];
			LinkCount += Node.Inputs.size();
			StringBytes += Node.DisplayName.size();
			EstimatedBytes += 64 + Node.DisplayName.size()
				+ Node.Inputs.size() * sizeof(FMaterialProgramLink);
			if (!Node.Id.IsValid())
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Schema,
					EMaterialProgramDiagnosticLocationKind::Node,
					Node.Id, 0, "Material program node GUID must be nonzero.");
			else if (!NodeIndices.emplace(Node.Id, Index).second)
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Schema,
					EMaterialProgramDiagnosticLocationKind::Node,
					Node.Id, 0, "Material program node GUID is duplicated.");
			if (!IsValidOpcode(Node.Opcode) || !IsValidValueType(Node.ResultType))
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Schema,
					EMaterialProgramDiagnosticLocationKind::Node,
					Node.Id, 0, "Material program node enum value is invalid.");
			if (Node.Inputs.size() > MaterialProgramMaxNodeInputCount)
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Bounds,
					EMaterialProgramDiagnosticLocationKind::Node,
					Node.Id, 0, "Material program node input count exceeds the bound.");
			if (Node.DisplayName.size() > MaterialProgramMaxDisplayNameBytes)
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Bounds,
					EMaterialProgramDiagnosticLocationKind::Node,
					Node.Id, 0, "Material program node display name exceeds the byte bound.");
		}
		if (LinkCount > MaterialProgramMaxLinkCount)
			AddDiagnostic(Diagnostics,
				EMaterialProgramDiagnosticCategory::Bounds,
				EMaterialProgramDiagnosticLocationKind::Program, {}, 0,
				"Material program link count exceeds the bound.");
		if (StringBytes > MaterialProgramMaxStringBytes)
			AddDiagnostic(Diagnostics,
				EMaterialProgramDiagnosticCategory::Bounds,
				EMaterialProgramDiagnosticLocationKind::Program, {}, 0,
				"Material program aggregate string bytes exceed the bound.");
		if (EstimatedBytes > MaterialProgramMaxCanonicalBytes)
			AddDiagnostic(Diagnostics,
				EMaterialProgramDiagnosticCategory::Bounds,
				EMaterialProgramDiagnosticLocationKind::Program, {}, 0,
				"Material program estimated canonical bytes exceed the bound.");
		if (std::ranges::any_of(Diagnostics, [](const auto& Diagnostic) {
			return Diagnostic.Category
				== EMaterialProgramDiagnosticCategory::Schema
				|| Diagnostic.Category
				== EMaterialProgramDiagnosticCategory::Bounds;
		}))
		{
			SortAndBoundDiagnostics(Diagnostics);
			return Result;
		}

		std::vector<size_t> OrderedIndices;
		OrderedIndices.reserve(NodeIndices.size());
		for (const auto& [Id, Index] : NodeIndices) OrderedIndices.push_back(Index);
		std::ranges::sort(OrderedIndices, [&](size_t A, size_t B) {
			return Program.Nodes[A].Id < Program.Nodes[B].Id;
		});

		for (size_t NodeIndex : OrderedIndices)
		{
			const FMaterialProgramNode& Node = Program.Nodes[NodeIndex];
			for (size_t InputIndex = 0;
				InputIndex < std::min<size_t>(
					Node.Inputs.size(), MaterialProgramMaxNodeInputCount);
				++InputIndex)
			{
				const FMaterialProgramLink& Link = Node.Inputs[InputIndex];
				if (Link.SourceOutputIndex != 0
					|| !NodeIndices.contains(Link.SourceNodeId))
					AddDiagnostic(Diagnostics,
						EMaterialProgramDiagnosticCategory::Graph,
						EMaterialProgramDiagnosticLocationKind::Input,
						Node.Id, static_cast<uint32>(InputIndex),
						"Material program input link is dangling or names an unsupported output slot.");
			}
		}

		std::unordered_set<FGuid> ReferencedParameters;
		for (size_t NodeIndex : OrderedIndices)
		{
			const FMaterialProgramNode& Node = Program.Nodes[NodeIndex];
			if (!IsValidOpcode(Node.Opcode) || !IsValidValueType(Node.ResultType))
				continue;
			ValidateNodeShape(Node, [&](size_t InputIndex) {
				if (InputIndex >= Node.Inputs.size()) return static_cast<const FMaterialProgramNode*>(nullptr);
				const auto It = NodeIndices.find(Node.Inputs[InputIndex].SourceNodeId);
				return It == NodeIndices.end() ? nullptr : &Program.Nodes[It->second];
			}, ParameterDefinitions, ReferencedParameters, Diagnostics);
		}
		if (ReferencedParameters.size()
			> MaterialProgramMaxReferencedParameterCount)
			AddDiagnostic(Diagnostics,
				EMaterialProgramDiagnosticCategory::Bounds,
				EMaterialProgramDiagnosticLocationKind::Program, {}, 0,
				"Material program referenced parameter count exceeds the bound.");

		std::vector<uint8> VisitState(Program.Nodes.size(), 0);
		std::vector<uint32> Depth(Program.Nodes.size(), 0);
		std::function<uint32(size_t)> Visit = [&](size_t Index) -> uint32 {
			if (VisitState[Index] == 2) return Depth[Index];
			if (VisitState[Index] == 1)
			{
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Graph,
					EMaterialProgramDiagnosticLocationKind::Node,
					Program.Nodes[Index].Id, 0,
					"Material program graph contains a cycle.");
				return 1;
			}
			VisitState[Index] = 1;
			uint32 MaximumInputDepth = 0;
			for (const FMaterialProgramLink& Link :
				Program.Nodes[Index].Inputs | std::views::take(
					MaterialProgramMaxNodeInputCount))
				if (const auto It = NodeIndices.find(Link.SourceNodeId);
					It != NodeIndices.end())
					MaximumInputDepth = std::max(
						MaximumInputDepth, Visit(It->second));
			VisitState[Index] = 2;
			Depth[Index] = MaximumInputDepth + 1;
			if (Depth[Index] > MaterialProgramMaxDepth)
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Bounds,
					EMaterialProgramDiagnosticLocationKind::Node,
					Program.Nodes[Index].Id, 0,
					"Material program dependency depth exceeds the bound.");
			return Depth[Index];
		};
		for (size_t Index : OrderedIndices)
			if (VisitState[Index] == 0) Visit(Index);

		const std::array<std::pair<EMaterialSurfaceOutput,
			const FMaterialProgramLink*>, 8> Outputs{{
			{EMaterialSurfaceOutput::BaseColor, &Program.Outputs.BaseColor},
			{EMaterialSurfaceOutput::Normal, &Program.Outputs.Normal},
			{EMaterialSurfaceOutput::Metallic, &Program.Outputs.Metallic},
			{EMaterialSurfaceOutput::Roughness, &Program.Outputs.Roughness},
			{EMaterialSurfaceOutput::AmbientOcclusion, &Program.Outputs.AmbientOcclusion},
			{EMaterialSurfaceOutput::Emissive, &Program.Outputs.Emissive},
			{EMaterialSurfaceOutput::Opacity, &Program.Outputs.Opacity},
			{EMaterialSurfaceOutput::OpacityMask, &Program.Outputs.OpacityMask}}};
		const bool bAggregate = Program.Outputs.Surface.SourceNodeId.IsValid();
		if (!bAggregate && Program.Outputs.Surface.SourceOutputIndex != 0)
			AddDiagnostic(Diagnostics, EMaterialProgramDiagnosticCategory::Graph,
				EMaterialProgramDiagnosticLocationKind::SurfaceOutput, {}, 8,
				"An unconnected aggregate Surface source has an invalid output slot.");
		if (bAggregate)
		{
			const auto It = NodeIndices.find(Program.Outputs.Surface.SourceNodeId);
			if (Program.Outputs.Surface.SourceOutputIndex != 0 || It == NodeIndices.end())
				AddDiagnostic(Diagnostics, EMaterialProgramDiagnosticCategory::Graph,
					EMaterialProgramDiagnosticLocationKind::SurfaceOutput, {}, 8,
					"Aggregate Surface source is missing or dangling.");
			else if (Program.Nodes[It->second].ResultType != EMaterialProgramValueType::Surface)
				AddDiagnostic(Diagnostics, EMaterialProgramDiagnosticCategory::Type,
					EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
					Program.Outputs.Surface.SourceNodeId, 8,
					"Aggregate Material Output requires a Surface value.");
		}
		for (const auto& [Output, Link] : Outputs)
		{
			const FMaterialProgramLiteral& Default =
				GetMaterialSurfaceOutputDefault(Program.Outputs, Output);
			const uint8 ComponentCount = GetComponentCount(
				GetMaterialSurfaceOutputType(Output));
			const std::array Components{Default.X, Default.Y, Default.Z, Default.W};
			if (!std::ranges::all_of(Components | std::views::take(ComponentCount),
				[](float Value) { return std::isfinite(Value); }))
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Schema,
					EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
					{}, static_cast<uint32>(Output),
					"Material surface output default contains a non-finite component.");
			if (!Link->SourceNodeId.IsValid())
			{
				if (Link->SourceOutputIndex != 0)
					AddDiagnostic(Diagnostics,
						EMaterialProgramDiagnosticCategory::Graph,
						EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
						{}, static_cast<uint32>(Output),
						"An unconnected material surface output has an invalid output slot.");
				continue;
			}
			if (bAggregate)
			{
				AddDiagnostic(Diagnostics, EMaterialProgramDiagnosticCategory::Graph,
					EMaterialProgramDiagnosticLocationKind::SurfaceOutput, {},
					static_cast<uint32>(Output),
					"Aggregate and per-property Material Output sources cannot coexist.");
				continue;
			}
			const auto It = NodeIndices.find(Link->SourceNodeId);
			if (Link->SourceOutputIndex != 0 || It == NodeIndices.end())
			{
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Graph,
					EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
					{}, static_cast<uint32>(Output),
					"Material surface output is missing or dangling.");
				continue;
			}
			if (Program.Nodes[It->second].ResultType
				!= GetMaterialSurfaceOutputType(Output))
				AddDiagnostic(Diagnostics,
					EMaterialProgramDiagnosticCategory::Type,
					EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
					Link->SourceNodeId, static_cast<uint32>(Output),
					"Material surface output type is incompatible with the fixed surface contract.");
		}

		SortAndBoundDiagnostics(Diagnostics);
		Result.bSucceeded = Diagnostics.empty();
		return Result;
	}

	auto SanitizeMaterialGraphPresentation(
		const FMaterialGraphPresentation& Presentation,
		const FMaterialProgram& Program) -> FMaterialGraphPresentation
	{
		FMaterialGraphPresentation Result;
		Result.Nodes.reserve(std::min<size_t>(
			Presentation.Nodes.size(), MaterialProgramMaxNodeCount));
		std::unordered_set<FGuid> LiveNodes;
		LiveNodes.reserve(Program.Nodes.size());
		for (const FMaterialProgramNode& Node : Program.Nodes)
			if (Node.Id.IsValid()) LiveNodes.insert(Node.Id);
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
		if (Presentation.bHasMaterialOutputPosition
			&& Presentation.MaterialOutputX >= -MaterialGraphPresentationCoordinateLimit
			&& Presentation.MaterialOutputX <= MaterialGraphPresentationCoordinateLimit
			&& Presentation.MaterialOutputY >= -MaterialGraphPresentationCoordinateLimit
			&& Presentation.MaterialOutputY <= MaterialGraphPresentationCoordinateLimit)
		{
			Result.bHasMaterialOutputPosition = true;
			Result.MaterialOutputX = Presentation.MaterialOutputX;
			Result.MaterialOutputY = Presentation.MaterialOutputY;
		}
		return Result;
	}

	auto InspectMaterialParameterDependencies(
		const FMaterialProgram& Program,
		std::span<const FMaterialParameterDefinition> Definitions)
		-> std::vector<FMaterialParameterDependency>
	{
		std::unordered_map<FGuid, const FMaterialProgramNode*> Nodes;
		Nodes.reserve(Program.Nodes.size());
		for (const FMaterialProgramNode& Node : Program.Nodes)
			Nodes.try_emplace(Node.Id, &Node);
		std::unordered_set<FGuid> VisitedNodes;
		std::unordered_set<FGuid> AddedParameters;
		std::vector<FMaterialParameterDependency> Result;
		auto Add = [&](const FGuid& SourceNodeId, const FGuid& ParameterId) {
			if (!ParameterId.IsValid()
				|| !AddedParameters.insert(ParameterId).second) return;
			const FMaterialParameterDefinition* Definition = FindParameter(
				Definitions, ParameterId);
			if (!Definition) return;
			Result.push_back({
				.SourceNodeId = SourceNodeId,
				.ParameterId = ParameterId,
				.Type = Definition->Type,
				.FirstUseOrder = static_cast<uint32>(Result.size()),
				.Name = Definition->Name,
				.DisplayName = Definition->DisplayName,
				.GroupName = Definition->GroupName,
				.SortOrder = Definition->SortOrder});
		};
		std::function<void(const FGuid&)> Visit = [&](const FGuid& Id) {
			if (!VisitedNodes.insert(Id).second) return;
			const auto It = Nodes.find(Id);
			if (It == Nodes.end()) return;
			const FMaterialProgramNode& Node = *It->second;
			for (const FMaterialProgramLink& Input : Node.Inputs)
				Visit(Input.SourceNodeId);
			if (Node.Opcode == EMaterialProgramOpcode::Parameter
				|| Node.Opcode == EMaterialProgramOpcode::TextureParameter)
				Add(Node.Id, Node.ParameterId);
		};
		if (Program.Outputs.Surface.SourceNodeId.IsValid())
			Visit(Program.Outputs.Surface.SourceNodeId);
		for (EMaterialSurfaceOutput Output : {
			EMaterialSurfaceOutput::BaseColor,
			EMaterialSurfaceOutput::Normal,
			EMaterialSurfaceOutput::Metallic,
			EMaterialSurfaceOutput::Roughness,
			EMaterialSurfaceOutput::AmbientOcclusion,
			EMaterialSurfaceOutput::Emissive,
			EMaterialSurfaceOutput::Opacity,
			EMaterialSurfaceOutput::OpacityMask})
		{
			const FGuid& Source = GetMaterialSurfaceOutputLink(
				Program.Outputs, Output).SourceNodeId;
			if (Source.IsValid()) Visit(Source);
		}
		return Result;
	}
}
