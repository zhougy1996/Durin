#include "Materials/MaterialTypes.h"

namespace Durin
{
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
			ETextureUsage TextureUsage = ETextureUsage::Color
		) -> FMaterialParameterDefinition
		{
			FMaterialParameterDefinition Result;
			Result.Id = Id;
			Result.Name = Name;
			Result.Type = Type;
			Result.Value = std::move(Value);
			Result.DisplayName = std::move(DisplayName);
			Result.GroupName = FName("Surface");
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
		Result.reserve(5);
		Result.push_back(MakeDefinition(BaseColorId, BaseColorName(), EMaterialParameterType::Vector,
			FMaterialParameterValue::MakeVector({0.95, 0.62, 0.22}), "Base Color", 0,
			EMaterialParameterPresentation::Color, true, 0.0f, 1.0f));
		Result.push_back(MakeDefinition(BaseColorTextureId, BaseColorTextureName(), EMaterialParameterType::Texture,
			FMaterialParameterValue::MakeTexture(nullptr), "Base Color Texture", 1,
			EMaterialParameterPresentation::AssetPicker, false, 0.0f, 0.0f, ETextureUsage::Color));
		Result.push_back(MakeDefinition(OpacityId, OpacityName(), EMaterialParameterType::Scalar,
			FMaterialParameterValue::MakeScalar(1.0f), "Opacity", 2,
			EMaterialParameterPresentation::Drag, true, 0.0f, 1.0f));
		Result.push_back(MakeDefinition(SpecularStrengthId, SpecularStrengthName(), EMaterialParameterType::Scalar,
			FMaterialParameterValue::MakeScalar(0.35f), "Specular Strength", 3,
			EMaterialParameterPresentation::Drag, true, 0.0f, 1.0f));
		Result.push_back(MakeDefinition(ShininessId, ShininessName(), EMaterialParameterType::Scalar,
			FMaterialParameterValue::MakeScalar(32.0f), "Shininess", 4,
			EMaterialParameterPresentation::Drag, true, 1.0f, 256.0f));
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
}
