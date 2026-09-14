#include "AssetForge/Builtins/ImportedSurfaceRecipe.h"

#include <algorithm>

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		using Type = EMaterialProgramValueType;
		using Op = EMaterialProgramOpcode;
		using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
		using Link = FMaterialProgramLink;
		auto SameFetch(const FImportedSurfaceSample& A, const FImportedSurfaceSample& B) -> bool
		{
			return A.ResourceIdentity == B.ResourceIdentity && A.Usage == B.Usage &&
				A.Sampler == B.Sampler && A.UVChannel == B.UVChannel && A.UVScale == B.UVScale &&
				A.UVOffset == B.UVOffset && A.UVRotation == B.UVRotation;
		}
	}

	auto MakeImportedSurfaceRecipe(const std::array<FImportedSurfaceRole, 8>& Roles)
		-> FImportedSurfaceRecipe
	{
		FImportedSurfaceRecipe Result;
		Result.CanonicalKey = "Durin.ImportedSurface:1";
		Result.Presentation.bHasMaterialOutputPosition = true;
		Result.Presentation.MaterialOutputX = 1000;
		const auto Definitions = MakePBRMaterialParameterDefinitions();
		std::vector<uint32> Groups;
		std::vector<Link> Samples;
		const auto Node = [&](Op Opcode, Type ValueType, std::vector<Link> Inputs, uint32 Role) {
			const FGuid Id{0x36b41f8e, 0x71984aca, Role, static_cast<uint32>(Result.Program.Nodes.size() + 1)};
			Result.Program.Nodes.push_back({.Id = Id, .Opcode = Opcode, .ResultType = ValueType,
				.Inputs = std::move(Inputs)});
			Result.Presentation.Nodes.push_back({Id, static_cast<int32>(Result.Program.Nodes.size() % 4) * 240,
				static_cast<int32>(Role) * 300});
			return Link{.SourceNodeId = Id};
		};
		const auto Parameter = [&](uint32 Role, Kind ParameterKind, Type ValueType) {
			const auto SurfaceRole = static_cast<EMaterialSurfaceOutput>(Role);
			const auto Id = GetMaterialSurfaceParameterId(SurfaceRole, ParameterKind);
			const auto Definition = std::ranges::find(Definitions, Id, &FMaterialParameterDefinition::Id);
			require(Definition != Definitions.end());
			const auto Link = Node(ParameterKind == Kind::Texture ? Op::TextureSampleParameter2D : Op::Parameter,
				ValueType, {}, Role);
			Result.Program.Nodes.back().Parameter = *Definition;
			Result.Owners.push_back({SurfaceRole, ParameterKind, Id});
			return Link;
		};
		for (uint32 I = 0; I < Roles.size(); ++I)
		{
			const auto Role = static_cast<EMaterialSurfaceOutput>(I);
			const auto ValueType = GetMaterialSurfaceOutputType(Role);
			const auto& Input = Roles[I];
			auto& Output = GetMaterialSurfaceOutputLink(Result.Program.Outputs, Role);
			Result.CanonicalKey += ";";
			if (!Input.Sample)
			{
				const bool bDefault = Input.Value == GetMaterialSurfaceOutputDefault(Result.Program.Outputs, Role);
				Result.CanonicalKey += bDefault ? "d" : "v";
				if (!bDefault) Output = Parameter(I, Kind::Value, ValueType);
				continue;
			}
			const auto& Sample = *Input.Sample;
			const auto Found = std::ranges::find_if(Groups, [&](uint32 Other) { return SameFetch(Sample, *Roles[Other].Sample); });
			const auto Group = static_cast<uint32>(Found - Groups.begin());
			const std::array UVValues{Sample.UVChannel, Sample.UVScale, Sample.UVOffset, Sample.UVRotation};
			const std::array UVDefaults{FMaterialProgramLiteral{}, FMaterialProgramLiteral{1, 1},
				FMaterialProgramLiteral{}, FMaterialProgramLiteral{}};
			constexpr std::array UVKinds{Kind::UVChannel, Kind::UVScale, Kind::UVOffset, Kind::UVRotation};
			constexpr std::array UVTypes{Type::Float, Type::Float2, Type::Float2, Type::Float};
			uint32 UVMask = 0;
			for (uint32 U = 0; U < 4; ++U) if (UVValues[U] != UVDefaults[U]) UVMask |= 1u << U;
			if (Found == Groups.end())
			{
				Link UV;
				if (UVMask)
				{
					std::vector<Link> Inputs(4);
					for (uint32 U = 0; U < 4; ++U)
						if (UVMask & (1u << U)) Inputs[U] = Parameter(I, UVKinds[U], UVTypes[U]);
					UV = Node(Op::TextureCoordinates, Type::Float2, std::move(Inputs), I);
				}
				auto Fetch = Parameter(I, Kind::Texture, Type::Float4);
				Result.Program.Nodes.back().Parameter.TextureUsage = Sample.Usage;
				Result.Program.Nodes.back().Inputs = {UV};
				Groups.push_back(I);
				Samples.push_back(Fetch);
			}
			else
			{
				const auto OwnerRole = static_cast<EMaterialSurfaceOutput>(Groups[Group]);
				Result.Owners.push_back({Role, Kind::Texture, GetMaterialSurfaceParameterId(OwnerRole, Kind::Texture)});
				for (uint32 U = 0; U < 4; ++U)
					if (UVMask & (1u << U)) Result.Owners.push_back({Role, UVKinds[U], GetMaterialSurfaceParameterId(OwnerRole, UVKinds[U])});
			}
			Output = Samples[Group];
			Output.SourceOutputIndex = Sample.bDecodeNormal ? 6 : Sample.OutputIndex;
			if (Sample.bDecodeNormal) Output = Node(Op::DecodeNormalRG, Type::Float3, {Output}, I);
			const FMaterialProgramLiteral Identity = ValueType == Type::Float3 ? FMaterialProgramLiteral{1, 1, 1} : FMaterialProgramLiteral{1};
			const bool bFactor = !Sample.bDecodeNormal && Input.Value != Identity;
			if (bFactor)
			{
				const auto Factor = Parameter(I, Kind::Value, ValueType);
				Output = Node(Op::Multiply, ValueType, {Output, Factor}, I);
			}
			Result.CanonicalKey += "s" + std::to_string(Group) + "," + std::to_string(Sample.OutputIndex) +
				"," + std::to_string(static_cast<uint32>(Sample.Usage)) + "," + std::to_string(UVMask) +
				"," + (Sample.bDecodeNormal ? "n" : "c") + "," + (bFactor ? "f" : "i");
		}
		return Result;
	}
}
