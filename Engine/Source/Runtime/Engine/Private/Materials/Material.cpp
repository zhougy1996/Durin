#include "Materials/Material.h"

namespace Durin
{
	DMaterial::DMaterial(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, ParameterDefinitions(MakeCanonicalMaterialParameterDefinitions())
	{
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

	auto DMaterial::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.ScalarValue;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
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
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
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
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
		return true;
	}

	auto DMaterial::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		OutValue = Definition->Value.ScalarValue;
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

	auto DMaterial::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		return ValidateCanonicalMaterialParameterDefinitions(ParameterDefinitions, OutError);
	}
}
