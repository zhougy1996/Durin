#include "Materials/MaterialTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialRenderTypes.h"

namespace Durin
{
	auto FMaterialPropertyOverrides::ApplyTo(FMaterialStaticProperties& Properties) const -> void
	{
		if (bOverrideBlendMode) Properties.BlendMode = Values.BlendMode;
		if (bOverrideShadingModel) Properties.ShadingModel = Values.ShadingModel;
		if (bOverrideOpacityMaskThreshold) Properties.OpacityMaskThreshold = Values.OpacityMaskThreshold;
		if (bOverrideTwoSided) Properties.bTwoSided = Values.bTwoSided;
		if (bOverrideDepthWritePolicy) Properties.DepthWritePolicy = Values.DepthWritePolicy;
	}

	auto CanonicalizeMaterialShaderProperties(FMaterialStaticProperties Properties)
		-> FMaterialStaticProperties
	{
		if (Properties.BlendMode != EMaterialBlendMode::Masked)
			Properties.OpacityMaskThreshold = FMaterialStaticProperties{}.OpacityMaskThreshold;
		else if (Properties.OpacityMaskThreshold == 0.0f)
			Properties.OpacityMaskThreshold = 0.0f;
		Properties.bTwoSided = false;
		Properties.DepthWritePolicy = EMaterialDepthWritePolicy::Automatic;
		return Properties;
	}

	auto FMaterialParameterValue::GetType() const -> EMaterialParameterType
	{
		return std::visit([]<typename T>(const T&) {
			if constexpr (std::is_same_v<T, float>) return EMaterialParameterType::Scalar;
			else if constexpr (std::is_same_v<T, FVector2>) return EMaterialParameterType::Vector2;
			else if constexpr (std::is_same_v<T, FVector3>) return EMaterialParameterType::Vector;
			else if constexpr (std::is_same_v<T, FVector4>) return EMaterialParameterType::Vector4;
			else return EMaterialParameterType::Texture;
		}, Value);
	}

	auto FMaterialParameterValue::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		if (auto* Texture = std::get_if<FMaterialTextureValue>(&Value))
		{
			DObject* Object = Texture->Texture.Get();
			Collector.AddReferencedObject(Object);
			Texture->Texture = Cast<DTexture2D>(Object);
		}
	}

	auto FMaterialParameterValue::MakeScalar(float Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.Value = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeVector2(const FVector2& Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.Value = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeVector(const FVector3& Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.Value = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeVector4(const FVector4& Value) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.Value = Value;
		return Result;
	}

	auto FMaterialParameterValue::MakeTexture(DTexture2D* Value, FMaterialSamplerState Sampler,
		EMaterialTextureFallback Fallback) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.Value = FMaterialTextureValue{Value, Sampler, Fallback};
		return Result;
	}

	auto IsValidMaterialSampling(FMaterialSamplerState State, EMaterialTextureFallback Fallback) -> bool
	{
		return State.MinFilter <= EMaterialSamplerMinFilter::LinearMipmapLinear
			&& State.MagFilter <= EMaterialSamplerMagFilter::Linear
			&& State.AddressU <= EMaterialSamplerAddressMode::ClampToEdge
			&& State.AddressV <= EMaterialSamplerAddressMode::ClampToEdge
			&& Fallback <= EMaterialTextureFallback::FlatRGNormal;
	}

	auto ValidateMaterialParameterDefinitions(
		std::span<const FMaterialParameterDefinition> Definitions) -> FMaterialParameterValidationResult
	{
		if (Definitions.size() > MaterialMaxParameterDefinitionCount)
		{
			return {.Error = EMaterialParameterError::TooManyDefinitions};
		}
		std::unordered_set<FGuid> Ids;
		std::unordered_set<FName> Names;
		for (const auto& Definition : Definitions)
		{
			if (!Definition.Id.IsValid())
				return {EMaterialParameterError::InvalidId, Definition.Id};
			if (!Ids.insert(Definition.Id).second)
				return {EMaterialParameterError::DuplicateId, Definition.Id};
			if (Definition.Name.IsNone())
				return {EMaterialParameterError::InvalidName, Definition.Id};
			if (!Names.insert(Definition.Name).second)
				return {EMaterialParameterError::DuplicateName, Definition.Id};
			if (Definition.Name.ToString().size() > MaterialMaxParameterTextBytes
				|| Definition.DisplayName.size() > MaterialMaxParameterTextBytes
				|| Definition.GroupName.ToString().size() > MaterialMaxParameterTextBytes
				|| Definition.DisplayName.find('\0') != std::string::npos)
			{
				return {EMaterialParameterError::InvalidText, Definition.Id};
			}
			if (Definition.Type != Definition.Value.GetType())
				return {EMaterialParameterError::InvalidType, Definition.Id};
			if (Definition.Type == EMaterialParameterType::Texture
				&& !IsValidMaterialSampling(Definition.Value.GetTexture().SamplerState, Definition.Value.GetTexture().TextureFallback))
				return {EMaterialParameterError::InvalidMetadata, Definition.Id};
			bool bFinite = false;
			switch (Definition.Type)
			{
			case EMaterialParameterType::Scalar:
				bFinite = std::isfinite(Definition.Value.GetScalar()); break;
			case EMaterialParameterType::Vector2:
				bFinite = std::isfinite(Definition.Value.GetVector2().x)
					&& std::isfinite(Definition.Value.GetVector2().y); break;
			case EMaterialParameterType::Vector:
				bFinite = std::isfinite(Definition.Value.GetVector().x)
					&& std::isfinite(Definition.Value.GetVector().y)
					&& std::isfinite(Definition.Value.GetVector().z); break;
			case EMaterialParameterType::Texture:
				bFinite = true; break;
			case EMaterialParameterType::Vector4:
				bFinite = std::isfinite(Definition.Value.GetVector4().x)
					&& std::isfinite(Definition.Value.GetVector4().y)
					&& std::isfinite(Definition.Value.GetVector4().z)
					&& std::isfinite(Definition.Value.GetVector4().w); break;
			default:
				return {EMaterialParameterError::InvalidType, Definition.Id};
			}
			if (!bFinite)
				return {EMaterialParameterError::InvalidDefault, Definition.Id};
			if (Definition.Presentation > EMaterialParameterPresentation::AssetPicker
				|| Definition.TextureUsage > ETextureUsage::DataMask
				|| (Definition.bHasRange
					&& (!std::isfinite(Definition.MinimumValue)
						|| !std::isfinite(Definition.MaximumValue)
						|| Definition.MinimumValue > Definition.MaximumValue)))
			{
				return {EMaterialParameterError::InvalidMetadata, Definition.Id};
			}
		}
		return {};
	}

	auto ValidateMaterialStaticProperties(
		const FMaterialStaticProperties& Properties) -> FMaterialOperationResult
	{
		switch (Properties.BlendMode)
		{
		case EMaterialBlendMode::Opaque:
		case EMaterialBlendMode::Masked:
		case EMaterialBlendMode::Translucent:
			break;
		default:
			return {EMaterialPropertyError::BlendModeInvalid};
		}
		switch (Properties.ShadingModel)
		{
		case EMaterialShadingModel::Lit:
		case EMaterialShadingModel::Unlit:
			break;
		default:
			return {EMaterialPropertyError::ShadingModelInvalid};
		}
		switch (Properties.DepthWritePolicy)
		{
		case EMaterialDepthWritePolicy::Automatic:
		case EMaterialDepthWritePolicy::Enabled:
		case EMaterialDepthWritePolicy::Disabled:
			break;
		default:
			return {EMaterialPropertyError::DepthWritePolicyInvalid};
		}
		if (!std::isfinite(Properties.OpacityMaskThreshold)
			|| Properties.OpacityMaskThreshold < 0.0f
			|| Properties.OpacityMaskThreshold > 1.0f)
		{
			return {EMaterialPropertyError::InvalidOpacityMaskThreshold};
		}
		return {};
	}
}
