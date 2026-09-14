#include "AssetForge/Builtins/PBRSurfaceMaterial.h"

namespace Durin::AssetForge::Builtins
{
	auto MakePBRSurfaceMaterialMRProgram(FMaterialGraphPresentation& OutPresentation) -> FMaterialProgram
	{
		using Type = EMaterialProgramValueType;
		using Op = EMaterialProgramOpcode;
		using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
		FMaterialProgram Program;
		FMaterialGraphPresentation Presentation{.bHasMaterialOutputPosition = true,
			.MaterialOutputX = 1600, .MaterialOutputY = 400};
		const auto Definitions = MakePBRMaterialParameterDefinitions();
		// RGB for color, decoded RG for normals, B/G/R for metallic/roughness/AO.
		constexpr std::array<uint8, 8> Channels{1, 8, 4, 3, 2, 1, 5, 2};
		const auto Node = [&](Op Opcode, Type ValueType, std::vector<FMaterialProgramLink> Inputs,
			uint32 Role, int32 X, int32 Y) {
			const FGuid Id{0x7f53711b, 0x48249b21, Role, static_cast<uint32>(Program.Nodes.size() + 1)};
			Program.Nodes.push_back({.Id = Id, .Opcode = Opcode, .ResultType = ValueType, .Inputs = std::move(Inputs)});
			Presentation.Nodes.push_back({Id, X, static_cast<int32>(Role) * 650 + Y});
			return FMaterialProgramLink{.SourceNodeId = Id};
		};
		for (uint32 I = 0; I < Channels.size(); ++I)
		{
			const auto Role = static_cast<EMaterialSurfaceOutput>(I);
			const auto Parameter = [&](Kind ParameterKind, Type ValueType, int32 X, int32 Y) {
				const auto Id = GetMaterialSurfaceParameterId(Role, ParameterKind);
				const auto Definition = std::ranges::find(Definitions, Id, &FMaterialParameterDefinition::Id);
				require(Definition != Definitions.end());
				const auto Link = Node(ParameterKind == Kind::Texture ? Op::TextureSampleParameter2D : Op::Parameter,
					ValueType, {}, I, X, Y);
				Program.Nodes.back().Parameter = *Definition;
				return Link;
			};
			const auto Factor = Parameter(Kind::Value, GetMaterialSurfaceOutputType(Role), 640, 0);
			const auto Channel = Parameter(Kind::UVChannel, Type::Float, -640, 0);
			const auto Scale = Parameter(Kind::UVScale, Type::Float2, -640, 130);
			const auto Offset = Parameter(Kind::UVOffset, Type::Float2, -640, 260);
			const auto Rotation = Parameter(Kind::UVRotation, Type::Float, -640, 390);
			const auto UV = Node(Op::TextureCoordinates, Type::Float2, {Channel, Scale, Offset, Rotation}, I, -320, 0);
			auto Sample = Parameter(Kind::Texture, Type::Float4, 0, 0);
			Program.Nodes.back().Inputs = {UV};
			Sample.SourceOutputIndex = Channels[I];
			GetMaterialSurfaceOutputLink(Program.Outputs, Role) = Node(
				I == 1 ? Op::BlendNormalsRNM : I == 5 ? Op::Add : Op::Multiply,
				GetMaterialSurfaceOutputType(Role), {Factor, Sample}, I, 960, 0);
		}
		OutPresentation = std::move(Presentation);
		return Program;
	}
}
