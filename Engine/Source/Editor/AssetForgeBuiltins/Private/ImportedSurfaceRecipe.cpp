#include "AssetForge/Builtins/ImportedSurfaceRecipe.h"

#include <algorithm>
#include "MaterialExpressionRecipeBuilder.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		using Type = EMaterialProgramValueType;
		using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
		using Link = FMaterialExpressionInput;
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
		Result.CanonicalKey = "Durin.ImportedSurface:4";
		Result.Graph.OutputPosition = {1000, 0};
		const auto Definitions = MakePBRMaterialParameterDefinitions();
		std::vector<uint32> Groups;
		std::vector<Link> Samples;
		Private::FMaterialExpressionRecipeBuilder Builder{Result.Graph, 0x36b41f8e, 0x71984aca};
		const auto PositionX = [&] { return static_cast<int32>((Result.Graph.Expressions.size() + 1) % 4) * 240; };
		const auto Parameter = [&](uint32 Role, Kind ParameterKind) {
			const auto SurfaceRole = static_cast<EMaterialSurfaceOutput>(Role);
			const auto Id = GetMaterialSurfaceParameterId(SurfaceRole, ParameterKind);
			const auto Definition = std::ranges::find(Definitions, Id, &FMaterialParameterDefinition::Id);
			require(Definition != Definitions.end());
			auto* Expression = Builder.Parameter(*Definition, Role, PositionX(), static_cast<int32>(Role) * 300, ParameterKind == Kind::UVScale || ParameterKind == Kind::UVOffset
					? EMaterialProgramValueType::Float2 : ParameterKind == Kind::Value ? GetMaterialSurfaceOutputType(SurfaceRole) : EMaterialProgramValueType::Float4);
			Result.Owners.push_back({SurfaceRole, ParameterKind, Id});
			return Link{Expression->Id};
		};
		for (uint32 I = 0; I < Roles.size(); ++I)
		{
			const auto Role = static_cast<EMaterialSurfaceOutput>(I);
			const auto ValueType = GetMaterialSurfaceOutputType(Role);
			const auto& Input = Roles[I];
			auto& Output = Builder.Output(Result.Graph.Outputs, I);
			Result.CanonicalKey += ";";
			if (!Input.Sample)
			{
				const bool bDefault = Input.Value == GetMaterialSurfaceOutputDefault(FMaterialSurfaceOutputs{}, Role);
				Result.CanonicalKey += bDefault ? "d" : "v";
				if (!bDefault) Output = Parameter(I, Kind::Value);
				continue;
			}
			const auto& Sample = *Input.Sample;
			const auto Found = std::ranges::find_if(Groups, [&](uint32 Other) { return SameFetch(Sample, *Roles[Other].Sample); });
			const auto Group = static_cast<uint32>(Found - Groups.begin());
			const std::array UVValues{Sample.UVChannel, Sample.UVScale, Sample.UVOffset, Sample.UVRotation};
			const std::array UVDefaults{FMaterialProgramLiteral{}, FMaterialProgramLiteral{1, 1},
				FMaterialProgramLiteral{}, FMaterialProgramLiteral{}};
			constexpr std::array UVKinds{Kind::UVChannel, Kind::UVScale, Kind::UVOffset, Kind::UVRotation};
			uint32 UVMask = 0;
			for (uint32 U = 0; U < 4; ++U) if (UVValues[U] != UVDefaults[U]) UVMask |= 1u << U;
			if (Found == Groups.end())
			{
				Link UV;
				if (UVMask)
				{
					std::vector<Link> Inputs(4);
					for (uint32 U = 0; U < 4; ++U)
						if (UVMask & (1u << U)) Inputs[U] = Parameter(I, UVKinds[U]);
					UV = Builder.TransformUV(I, PositionX() - 2240, static_cast<int32>(I) * 900,
						Inputs[0], Inputs[1], Inputs[2], Inputs[3]);
				}
				auto Fetch = Parameter(I, Kind::Texture);
				auto* Texture = Cast<DMaterialExpressionTextureSampleParameter2D>(Result.Graph.Expressions.back().Get());
				Texture->TextureUsage = Sample.Usage; Texture->UV = UV;
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
			Output.OutputIndex = Sample.OutputIndex;
			const FMaterialProgramLiteral Identity = ValueType == Type::Float3 ? FMaterialProgramLiteral{1, 1, 1} : FMaterialProgramLiteral{1};
			const bool bFactor = I != 1 && Input.Value != Identity;
			if (bFactor)
			{
				const auto Factor = Parameter(I, Kind::Value);
				auto* Product = Builder.Add<DMaterialExpressionMultiply>(I, PositionX(), static_cast<int32>(I) * 300);
				Product->ResultType = ValueType; Product->A = Output; Product->B = Factor;
				Output = {Product->Id};
			}
			Result.CanonicalKey += "s" + std::to_string(Group) + "," + std::to_string(Sample.OutputIndex) +
				"," + std::to_string(static_cast<uint32>(Sample.Usage)) + "," + std::to_string(UVMask) +
				"," + (I == 1 ? "normal" : "c") + "," + (bFactor ? "f" : "i");
		}
		return Result;
	}
}
