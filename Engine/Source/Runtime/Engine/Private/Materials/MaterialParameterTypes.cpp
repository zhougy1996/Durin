#include "Materials/MaterialTypes.h"
#include "Materials/MaterialRenderTypes.h"

namespace Durin
{
	auto UpgradeMaterialParameterSchemaVersion(
		FMaterialParameterSchemaVersion& InOutVersion,
		std::string& OutWarning,
		std::string& OutError) -> bool
	{
		OutWarning.clear();
		OutError.clear();
		if (InOutVersion == 0)
		{
			InOutVersion = 1;
			OutWarning = "Material parameter schema version was missing; assumed version 1 and upgraded to the current version.";
		}
		if (InOutVersion == 1)
		{
			InOutVersion = 2;
			if (OutWarning.empty()) OutWarning = "Material parameter schema version 1 was upgraded to version 2; base SpecularStrength and Shininess values were discarded while instance overrides remain orphans.";
		}
		if (InOutVersion == 2)
		{
			InOutVersion = 3;
			if (OutWarning.empty()) OutWarning = "Material parameter schema version 2 was upgraded to version 3; UV transform values were migrated to Vector2.";
		}
		if (InOutVersion == 3)
		{
			InOutVersion = 4;
			if (OutWarning.empty()) OutWarning = "Material parameter schema version 3 was upgraded to version 4; UV rotation and per-texture sampler defaults were added.";
			return true;
		}
		if (InOutVersion == CurrentMaterialParameterSchemaVersion) return true;
		OutError = std::format(
			"Material parameter schema version {} is unsupported; expected {}.",
			InOutVersion, CurrentMaterialParameterSchemaVersion);
		return false;
	}

	auto FMaterialParameterValue::MakeScalar(float Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.ScalarValue = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeVector(const FVector3& Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.VectorValue = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeVector2(const FVector2& Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.Vector2Value = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeTexture(DTexture2D* Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.TextureValue = Value;
		return Result;
	}

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

		auto SpecularStrengthName() -> const FName&
		{
			static const FName Name("SpecularStrength");
			return Name;
		}

		auto ShininessName() -> const FName&
		{
			static const FName Name("Shininess");
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
			Result.bHasRange = bHasRange;
			Result.MinimumValue = MinimumValue;
			Result.MaximumValue = MaximumValue;
			Result.TextureUsage = TextureUsage;
			return Result;
		}

		auto HasCanonicalMetadata(
			const FMaterialParameterDefinition& Definition,
			const FMaterialParameterDefinition& Canonical
		) -> bool
		{
			return Definition.Id == Canonical.Id
				&& Definition.Name == Canonical.Name
				&& Definition.Type == Canonical.Type
				&& Definition.DisplayName == Canonical.DisplayName
				&& Definition.GroupName == Canonical.GroupName
				&& Definition.SortOrder == Canonical.SortOrder
				&& Definition.Presentation == Canonical.Presentation
				&& Definition.bHasRange == Canonical.bHasRange
				&& Definition.MinimumValue == Canonical.MinimumValue
				&& Definition.MaximumValue == Canonical.MaximumValue
				&& Definition.TextureUsage == Canonical.TextureUsage;
		}
	}

	auto MakeCanonicalMaterialParameterDefinitions() -> std::vector<FMaterialParameterDefinition>
	{
		using namespace MaterialParameters;
		std::vector<FMaterialParameterDefinition> Result;
		Result.reserve(56);
		const std::array ConstantIds{BaseColorId, NormalId, MetallicId, RoughnessId, AmbientOcclusionId, EmissiveId, OpacityId, OpacityMaskId};
		const std::array ConstantNames{&BaseColorName(), &NormalName(), &MetallicName(), &RoughnessName(), &AmbientOcclusionName(), &EmissiveName(), &OpacityName(), &OpacityMaskName()};
		const std::array TextureNames{&BaseColorTextureName(), &NormalTextureName(), &MetallicTextureName(), &RoughnessTextureName(), &AmbientOcclusionTextureName(), &EmissiveTextureName(), &OpacityTextureName(), &OpacityMaskTextureName()};
		const std::array RoleNames{"BaseColor", "Normal", "Metallic", "Roughness", "AmbientOcclusion", "Emissive", "Opacity", "OpacityMask"};
		const std::array DisplayNames{"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"};
		const std::array GroupNames{"Surface/Base", "Surface/Normal", "Surface/Metallic", "Surface/Roughness", "Surface/Ambient Occlusion", "Surface/Emissive", "Surface/Opacity", "Surface/Opacity Mask"};
		const std::array TextureUsages{ETextureUsage::Color, ETextureUsage::Normal, ETextureUsage::DataMask, ETextureUsage::DataMask, ETextureUsage::DataMask, ETextureUsage::Color, ETextureUsage::DataMask, ETextureUsage::DataMask};
		for (size_t Role = 0; Role < 8; ++Role)
		{
			const bool bVector = Role == 0 || Role == 1 || Role == 5;
			FMaterialParameterValue ConstantValue;
			float Minimum = 0.0f;
			float Maximum = 1.0f;
			if (Role == 0) ConstantValue = FMaterialParameterValue::MakeVector({0.95, 0.62, 0.22});
			else if (Role == 1) { ConstantValue = FMaterialParameterValue::MakeVector({0.0, 0.0, 1.0}); Minimum = -1.0f; }
			else if (Role == 3) ConstantValue = FMaterialParameterValue::MakeScalar(0.5f);
			else if (Role == 5) { ConstantValue = FMaterialParameterValue::MakeVector(FVector3(0.0)); Maximum = 64.0f; }
			else if (Role == 2) ConstantValue = FMaterialParameterValue::MakeScalar(0.0f);
			else ConstantValue = FMaterialParameterValue::MakeScalar(1.0f);
			const int32 Sort = static_cast<int32>(Role * 7);
			const FName Group(GroupNames[Role]);
			Result.push_back(MakeDefinition(ConstantIds[Role], *ConstantNames[Role], bVector ? EMaterialParameterType::Vector : EMaterialParameterType::Scalar,
				ConstantValue, DisplayNames[Role], Sort, (Role == 0 || Role == 5) ? EMaterialParameterPresentation::Color : EMaterialParameterPresentation::Drag,
				true, Minimum, Maximum, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(TextureIds[Role], *TextureNames[Role], EMaterialParameterType::Texture,
				FMaterialParameterValue::MakeTexture(nullptr), std::string(DisplayNames[Role]) + " Texture", Sort + 1,
				EMaterialParameterPresentation::AssetPicker, false, 0.0f, 0.0f, TextureUsages[Role], Group));
			Result.push_back(MakeDefinition(UVChannelIds[Role], FName(std::string(RoleNames[Role]) + "UVChannel"), EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(0.0f), "UV Channel", Sort + 2, EMaterialParameterPresentation::Integer,
				true, 0.0f, 3.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(UVScaleIds[Role], FName(std::string(RoleNames[Role]) + "UVScale"), EMaterialParameterType::Vector2,
				FMaterialParameterValue::MakeVector2({1.0, 1.0}), "UV Scale", Sort + 3, EMaterialParameterPresentation::Drag,
				true, -1024.0f, 1024.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(UVOffsetIds[Role], FName(std::string(RoleNames[Role]) + "UVOffset"), EMaterialParameterType::Vector2,
				FMaterialParameterValue::MakeVector2(FVector2(0.0)), "UV Offset", Sort + 4, EMaterialParameterPresentation::Drag,
				true, -1024.0f, 1024.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(UVRotationIds[Role], FName(std::string(RoleNames[Role]) + "UVRotation"), EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(0.0f), "UV Rotation (Radians)", Sort + 5, EMaterialParameterPresentation::Drag,
				true, -1024.0f, 1024.0f, ETextureUsage::Color, Group));
			Result.push_back(MakeDefinition(SamplerStateIds[Role], FName(std::string(RoleNames[Role]) + "SamplerState"), EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(EncodeMaterialSamplerState({})), "Sampler State", Sort + 6, EMaterialParameterPresentation::Integer,
				true, 0.0f, 255.0f, ETextureUsage::Color, Group));
		}
		return Result;
	}

	auto GetCanonicalMaterialParameterDefinitions() -> std::span<const FMaterialParameterDefinition>
	{
		static const std::vector<FMaterialParameterDefinition> Definitions = MakeCanonicalMaterialParameterDefinitions();
		return Definitions;
	}

	auto ValidateCanonicalMaterialParameterDefinitions(
		std::span<const FMaterialParameterDefinition> Definitions,
		std::string& OutError
	) -> bool
	{
		OutError.clear();
		const std::span Canonical = GetCanonicalMaterialParameterDefinitions();
		if (Definitions.size() != Canonical.size())
		{
			OutError = std::format("Material parameter schema contains {} definitions; expected {}.", Definitions.size(), Canonical.size());
			return false;
		}

		std::unordered_set<FGuid> Ids;
		std::unordered_set<FName> Names;
		for (size_t Index = 0; Index < Definitions.size(); ++Index)
		{
			const FMaterialParameterDefinition& Definition = Definitions[Index];
			if (!Definition.Id.IsValid())
			{
				OutError = std::format("Material parameter definition {} has an invalid GUID.", Index);
				return false;
			}
			if (!Ids.insert(Definition.Id).second)
			{
				OutError = std::format("Material parameter schema contains duplicate GUID {}.", Definition.Id.ToString());
				return false;
			}
			if (Definition.Name.IsNone())
			{
				OutError = std::format("Material parameter definition {} has a None name.", Index);
				return false;
			}
			if (!Names.insert(Definition.Name).second)
			{
				OutError = std::format("Material parameter schema contains duplicate name '{}'.", Definition.Name.ToString());
				return false;
			}
			if (!HasCanonicalMetadata(Definition, Canonical[Index]))
			{
				OutError = std::format("Material parameter definition {} ('{}') does not match the canonical identity, type, order, or metadata.",
					Index, Definition.Name.ToString());
				return false;
			}
		}
		return true;
	}

	auto ValidateMaterialStaticProperties(
		const FMaterialStaticProperties& Properties,
		std::string& OutError
	) -> bool
	{
		OutError.clear();
		switch (Properties.BlendMode)
		{
		case EMaterialBlendMode::Opaque:
		case EMaterialBlendMode::Masked:
		case EMaterialBlendMode::Translucent:
			break;
		default:
			OutError = "Material blend mode is invalid.";
			return false;
		}
		switch (Properties.ShadingModel)
		{
		case EMaterialShadingModel::Lit:
		case EMaterialShadingModel::Unlit:
			break;
		default:
			OutError = "Material shading model is invalid.";
			return false;
		}
		switch (Properties.DepthWritePolicy)
		{
		case EMaterialDepthWritePolicy::Automatic:
		case EMaterialDepthWritePolicy::Enabled:
		case EMaterialDepthWritePolicy::Disabled:
			break;
		default:
			OutError = "Material depth-write policy is invalid.";
			return false;
		}
		if (!std::isfinite(Properties.OpacityMaskThreshold)
			|| Properties.OpacityMaskThreshold < 0.0f
			|| Properties.OpacityMaskThreshold > 1.0f)
		{
			OutError = "Material opacity-mask threshold must be finite and in the inclusive range [0, 1].";
			return false;
		}
		return true;
	}
}
