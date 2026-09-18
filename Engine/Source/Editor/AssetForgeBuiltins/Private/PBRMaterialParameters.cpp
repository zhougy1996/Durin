#include "AssetForge/Builtins/PBRMaterialParameters.h"

namespace Durin::AssetForge::Builtins
{
	namespace MaterialParameters
	{
		auto BaseColorName() -> const FName&
		{
			static const FName Name("BaseColor");
			return Name;
		}

		auto BaseColorTextureName() -> const FName&
		{
			static const FName Name("BaseColorTexture");
			return Name;
		}

		auto OpacityName() -> const FName&
		{
			static const FName Name("Opacity");
			return Name;
		}

#define DURIN_DEFINE_MATERIAL_PARAMETER_NAME(FunctionName, Literal) \
		auto FunctionName() -> const FName& { static const FName Name(Literal); return Name; }
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(NormalName, "Normal")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(NormalTextureName, "NormalTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(MetallicName, "Metallic")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(MetallicTextureName, "MetallicTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(RoughnessName, "Roughness")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(RoughnessTextureName, "RoughnessTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(AmbientOcclusionName, "AmbientOcclusion")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(AmbientOcclusionTextureName, "AmbientOcclusionTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(EmissiveName, "Emissive")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(EmissiveTextureName, "EmissiveTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(OpacityTextureName, "OpacityTexture")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(OpacityMaskName, "OpacityMask")
		DURIN_DEFINE_MATERIAL_PARAMETER_NAME(OpacityMaskTextureName, "OpacityMaskTexture")
#undef DURIN_DEFINE_MATERIAL_PARAMETER_NAME
	}

	namespace
	{
		auto MakeDefinition(
			FGuid Id,
			FName Name,
			EMaterialParameterType Type,
			FMaterialParameterValue Value,
			std::string DisplayName,
			int32 SortOrder,
			EMaterialParameterPresentation Presentation,
			bool bHasRange = false,
			float MinimumValue = 0.0f,
			float MaximumValue = 0.0f,
			ETextureUsage TextureUsage = ETextureUsage::Color,
			FName GroupName = FName("Surface")
		) -> FMaterialParameterDefinition
		{
			FMaterialParameterDefinition Result;
			Result.Id = Id;
			Result.Name = Name;
			Result.Type = Type;
			Result.Value = std::move(Value);
			Result.DisplayName = std::move(DisplayName);
			Result.GroupName = GroupName;
			Result.SortOrder = SortOrder;
			Result.Presentation = Presentation;
			Result.bHasRange = Type == EMaterialParameterType::Scalar && bHasRange;
			Result.MinimumValue = Type == EMaterialParameterType::Scalar ? MinimumValue : 0.0f;
			Result.MaximumValue = Type == EMaterialParameterType::Scalar ? MaximumValue : 0.0f;
			Result.TextureUsage = TextureUsage;
			return Result;
		}


	}

	auto MakePBRMaterialParameterDefinitions() -> std::vector<FMaterialParameterDefinition>
	{
		using namespace MaterialParameters;
		std::vector<FMaterialParameterDefinition> Result;
		Result.reserve(BuiltinParameterRoleCount * BuiltinParameterKindCount);
		const std::array ConstantNames{&BaseColorName(), &NormalName(), &MetallicName(), &RoughnessName(), &AmbientOcclusionName(), &EmissiveName(), &OpacityName(), &OpacityMaskName()};
		const std::array TextureNames{&BaseColorTextureName(), &NormalTextureName(), &MetallicTextureName(), &RoughnessTextureName(), &AmbientOcclusionTextureName(), &EmissiveTextureName(), &OpacityTextureName(), &OpacityMaskTextureName()};
		const std::array RoleNames{"BaseColor", "Normal", "Metallic", "Roughness", "AmbientOcclusion", "Emissive", "Opacity", "OpacityMask"};
		const std::array DisplayNames{"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"};
		const std::array GroupNames{"Surface/Base", "Surface/Normal", "Surface/Metallic", "Surface/Roughness", "Surface/Ambient Occlusion", "Surface/Emissive", "Surface/Opacity", "Surface/Opacity Mask"};
		const std::array TextureUsages{ETextureUsage::Color, ETextureUsage::Normal, ETextureUsage::DataMask, ETextureUsage::DataMask, ETextureUsage::DataMask, ETextureUsage::Color, ETextureUsage::DataMask, ETextureUsage::DataMask};
		for (size_t Role = 0; Role < BuiltinParameterRoleCount; ++Role)
		{
			const auto BuiltinRole = static_cast<EMaterialBuiltinParameterRole>(Role);
			const bool bVector = Role == 0 || Role == 1 || Role == 5;
			FMaterialParameterValue ConstantValue;
			float Minimum = 0.0f;
			float Maximum = 1.0f;
			if (Role == 0) ConstantValue = FMaterialParameterValue::MakeVector4({0.5, 0.5, 0.5, 0});
			else if (Role == 1) { ConstantValue = FMaterialParameterValue::MakeVector4({0.0, 0.0, 1.0, 0}); Minimum = -1.0f; }
			else if (Role == 3) ConstantValue = FMaterialParameterValue::MakeScalar(0.5f);
			else if (Role == 5) { ConstantValue = FMaterialParameterValue::MakeVector4(FVector4(0.0)); Maximum = 64.0f; }
			else if (Role == 2) ConstantValue = FMaterialParameterValue::MakeScalar(0.0f);
			else ConstantValue = FMaterialParameterValue::MakeScalar(1.0f);
			const int32 Sort = static_cast<int32>(Role * 7);
			const FName Group(GroupNames[Role]);
			Result.push_back(MakeDefinition(GetBuiltinParameterId(BuiltinRole, EMaterialBuiltinParameterKind::Value), *ConstantNames[Role], bVector ? EMaterialParameterType::Vector4 : EMaterialParameterType::Scalar,
				ConstantValue, DisplayNames[Role], Sort, (Role == 0 || Role == 5) ? EMaterialParameterPresentation::Color : EMaterialParameterPresentation::Drag,
				true, Minimum, Maximum, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(GetBuiltinParameterId(BuiltinRole, EMaterialBuiltinParameterKind::Texture), *TextureNames[Role], EMaterialParameterType::Texture,
				FMaterialParameterValue::MakeTexture(nullptr), std::string(DisplayNames[Role]) + " Texture", Sort + 1,
				EMaterialParameterPresentation::AssetPicker, false, 0.0f, 0.0f, TextureUsages[Role], Group));
			Result.push_back(MakeDefinition(GetBuiltinParameterId(BuiltinRole, EMaterialBuiltinParameterKind::UVChannel), FName(std::string(RoleNames[Role]) + "UVChannel"), EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(0.0f), "UV Channel", Sort + 2, EMaterialParameterPresentation::Integer,
				true, 0.0f, 3.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(GetBuiltinParameterId(BuiltinRole, EMaterialBuiltinParameterKind::UVScale), FName(std::string(RoleNames[Role]) + "UVScale"), EMaterialParameterType::Vector4,
				FMaterialParameterValue::MakeVector4({1.0, 1.0, 0, 0}), "UV Scale", Sort + 3, EMaterialParameterPresentation::Drag,
				true, -1024.0f, 1024.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(GetBuiltinParameterId(BuiltinRole, EMaterialBuiltinParameterKind::UVOffset), FName(std::string(RoleNames[Role]) + "UVOffset"), EMaterialParameterType::Vector4,
				FMaterialParameterValue::MakeVector4(FVector4(0.0)), "UV Offset", Sort + 4, EMaterialParameterPresentation::Drag,
				true, -1024.0f, 1024.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(GetBuiltinParameterId(BuiltinRole, EMaterialBuiltinParameterKind::UVRotation), FName(std::string(RoleNames[Role]) + "UVRotation"), EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(0.0f), "UV Rotation (Radians)", Sort + 5, EMaterialParameterPresentation::Drag,
				true, -1024.0f, 1024.0f, ETextureUsage::Color, Group));

		}
		for (FMaterialParameterDefinition& Definition : Result)
		{
			if (Definition.Type != EMaterialParameterType::Texture) continue;
			const auto Role = MaterialParameters::FindBuiltinParameterRole(
				Definition.Id,
				MaterialParameters::EMaterialBuiltinParameterKind::Texture);
			Definition.Value.GetTexture().TextureFallback = Role
				== MaterialParameters::EMaterialBuiltinParameterRole::Normal
				? EMaterialTextureFallback::FlatRGNormal
				: (Role
					== MaterialParameters::EMaterialBuiltinParameterRole::Emissive
					? EMaterialTextureFallback::Black
					: EMaterialTextureFallback::White);
		}
		return Result;
	}

	auto GetPBRMaterialParameterDefinitions() -> std::span<const FMaterialParameterDefinition>
	{
		static const std::vector<FMaterialParameterDefinition> Definitions = MakePBRMaterialParameterDefinitions();
		return Definitions;
	}


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

}
