#include "Materials/Material.h"

namespace Durin
{
	DMaterial::DMaterial(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, ParameterSchemaVersion(CurrentMaterialParameterSchemaVersion)
		, ParameterDefinitions(MakeCanonicalMaterialParameterDefinitions())
	{
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose)) PublishMaterialRenderProxyState();
	}

	auto DMaterial::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		return ParameterDefinitions;
	}

	auto DMaterial::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition) return false;
		OutParameter.Definition = Definition;
		OutParameter.Value = Definition->Value;
		OutParameter.Source = const_cast<DMaterial*>(this);
		OutParameter.bHasLocalOverride = false;
		return true;
	}

	auto DMaterial::SetStaticProperties(const FMaterialStaticProperties& InProperties) -> bool
	{
		std::string Error;
		if (!ValidateMaterialStaticProperties(InProperties, Error)) return false;
		if (StaticProperties == InProperties) return true;
		StaticProperties = InProperties;
		MarkPackageDirty();
		MarkRenderDataDirty(
			EMaterialRenderDirtyFlags::ShaderMap
				| EMaterialRenderDirtyFlags::PipelineState);
		return true;
	}

	auto DMaterial::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.ScalarValue;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.Vector2Value;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.VectorValue;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.TextureValue;
		if (Mutable.Get() == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		OutValue = Definition->Value.ScalarValue;
		return true;
	}

	auto DMaterial::GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		OutValue = Definition->Value.Vector2Value;
		return true;
	}

	auto DMaterial::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		OutValue = Definition->Value.VectorValue;
		return true;
	}

	auto DMaterial::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		OutValue = Definition->Value.TextureValue.Get();
		return true;
	}

	auto DMaterial::BuildMaterialLocalRenderLayer() const
		-> FMaterialLocalRenderLayer
	{
		FMaterialLocalRenderLayer Result;
		Result.Parameters.reserve(ParameterDefinitions.size());
		for (const FMaterialParameterDefinition& Definition
			: ParameterDefinitions)
		{
			Result.Parameters.push_back(
				BuildMaterialLocalRenderParameter(
					Definition.Id,
					Definition.Type,
					Definition.Value));
		}
		Result.StaticProperties = StaticProperties;
		return Result;
	}

	auto DMaterial::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		const FMaterialParameterSchemaVersion LoadedSchemaVersion = ParameterSchemaVersion;
		const bool bUpgradeDefinitions = ParameterSchemaVersion < CurrentMaterialParameterSchemaVersion;
		std::string SchemaWarning;
		if (!UpgradeMaterialParameterSchemaVersion(
				ParameterSchemaVersion, SchemaWarning, OutError))
		{
			return false;
		}
		if (bUpgradeDefinitions)
		{
			std::vector<FMaterialParameterDefinition> Upgraded = MakeCanonicalMaterialParameterDefinitions();
			if (LoadedSchemaVersion >= 2)
			{
				for (FMaterialParameterDefinition& New : Upgraded)
				{
					const auto Old = std::ranges::find(ParameterDefinitions, New.Id, &FMaterialParameterDefinition::Id);
					if (Old == ParameterDefinitions.end()) continue;
					if (Old->Type == New.Type)
					{
						New.Value = Old->Value;
					}
					else if (Old->Type == EMaterialParameterType::Vector
						&& New.Type == EMaterialParameterType::Vector2)
					{
						New.Value = FMaterialParameterValue::MakeVector2(
							FVector2(Old->Value.VectorValue));
					}
				}
			}
			else
			{
				for (const FGuid& PreservedId : {MaterialParameters::BaseColorId, MaterialParameters::BaseColorTextureId, MaterialParameters::OpacityId})
				{
					const auto Old = std::ranges::find(ParameterDefinitions, PreservedId, &FMaterialParameterDefinition::Id);
					const auto New = std::ranges::find(Upgraded, PreservedId, &FMaterialParameterDefinition::Id);
					if (Old != ParameterDefinitions.end() && New != Upgraded.end() && Old->Type == New->Type)
						New->Value = Old->Value;
				}
			}
			ParameterDefinitions = std::move(Upgraded);
		}
		if (!ValidateCanonicalMaterialParameterDefinitions(
				ParameterDefinitions, OutError)
			|| !ValidateMaterialStaticProperties(
				StaticProperties, OutError))
		{
			return false;
		}
		PublishMaterialRenderProxyState();
		return true;
	}
}
