#include "Materials/MaterialProgramCompiler.h"
#include "MaterialParameterValidation.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialParameterValues.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialRenderTypes.h"

namespace Durin
{
	auto FMaterialParameterValue::ResolveTextureReference() -> void
	{
		if (GetType() != EMaterialParameterType::Texture) return;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		auto& Texture = GetTexture();
		if (Texture.Texture) Texture.TextureReference = Texture.Texture->GetTextureReferenceRHI();
		Texture.Texture = nullptr;
	}

	auto FMaterialParameterValue::GetPayloadBytes() const -> uint64
	{
		switch (GetType()) {
		case EMaterialParameterType::Scalar: return sizeof(float);
		case EMaterialParameterType::Vector2: return sizeof(FVector2);
		case EMaterialParameterType::Vector: return sizeof(FVector3);
		case EMaterialParameterType::Vector4: return sizeof(FVector4);
		case EMaterialParameterType::Texture: return sizeof(FRHITextureReferenceRef) + sizeof(FMaterialSamplerState) + sizeof(EMaterialTextureFallback) + sizeof(EMaterialTextureDimension);
		}
		return 0;
	}

	auto FMaterialInstanceParameterSet::SetParameter(const FGuid& Id, const FMaterialParameterValue& Value) -> void
	{
		// One identity has one concrete type, including after native layout changes.
		const auto EraseOther = [&](auto& Parameters, EMaterialParameterType Type) {
			if (Value.GetType() != Type) std::erase_if(Parameters, [&](const auto& Parameter) { return Parameter.ParameterId == Id; });
		};
		EraseOther(ScalarParameters, EMaterialParameterType::Scalar);
		EraseOther(VectorParameters, EMaterialParameterType::Vector4);
		EraseOther(TextureParameters, EMaterialParameterType::Texture);
		if (Value.GetType() != EMaterialParameterType::Vector2) std::erase_if(Vector2Parameters, [&](const auto& Parameter) { return Parameter.first == Id; });
		if (Value.GetType() != EMaterialParameterType::Vector) std::erase_if(Vector3Parameters, [&](const auto& Parameter) { return Parameter.first == Id; });
		const auto Set = [&](auto& Parameters) {
			using TParameter = typename std::decay_t<decltype(Parameters)>::value_type;
			auto It = std::ranges::find(Parameters, Id, &TParameter::ParameterId);
			if (It == Parameters.end() || It->ParameterId != Id) {
				It = Parameters.emplace(It);
				It->ParameterId = Id;
			}
			It->SetValue(Value);
		};
		const auto SetVector = [&]<typename T>(std::vector<std::pair<FGuid, T>>& Parameters, const T& Vector) {
			auto It = std::ranges::find(Parameters, Id, &std::pair<FGuid, T>::first);
			if (It != Parameters.end() && It->first == Id) It->second = Vector;
			else Parameters.emplace(It, Id, Vector);
		};
		switch (Value.GetType()) {
		case EMaterialParameterType::Scalar: Set(ScalarParameters); break;
		case EMaterialParameterType::Vector2: SetVector(Vector2Parameters, Value.GetVector2()); break;
		case EMaterialParameterType::Vector: SetVector(Vector3Parameters, Value.GetVector()); break;
		case EMaterialParameterType::Vector4: Set(VectorParameters); break;
		case EMaterialParameterType::Texture: Set(TextureParameters); break;
		}
	}

	auto FMaterialInstanceParameterSet::GetParameterValue(const FGuid& Id, FMaterialParameterValue& OutValue) const -> bool
	{
		const auto Find = [&](const auto& Parameters) {
			using TParameter = typename std::decay_t<decltype(Parameters)>::value_type;
			const auto It = std::ranges::find(Parameters, Id, &TParameter::ParameterId);
			if (It == Parameters.end() || It->ParameterId != Id) return false;
			OutValue = It->GetValue();
			return true;
		};
		if (Find(ScalarParameters) || Find(VectorParameters) || Find(TextureParameters)) return true;
		const auto FindVector = [&]<typename T>(const std::vector<std::pair<FGuid, T>>& Parameters) {
			const auto It = std::ranges::find(Parameters, Id, &std::pair<FGuid, T>::first);
			if (It == Parameters.end() || It->first != Id) return false;
			OutValue = FMaterialParameterValue(It->second);
			return true;
		};
		return FindVector(Vector2Parameters) || FindVector(Vector3Parameters);
	}

	auto FMaterialInstanceParameterSet::ResolveTextureReferences() -> void
	{
		for (auto& Parameter : TextureParameters) {
			auto Value = Parameter.GetValue();
			Value.ResolveTextureReference();
			Parameter.SetValue(Value);
		}
	}

	auto FMaterialInstanceParameterSet::GetPayloadBytes() const -> uint64
	{
		return ScalarParameters.size() * sizeof(float) + Vector2Parameters.size() * sizeof(FVector2)
			+ Vector3Parameters.size() * sizeof(FVector3) + VectorParameters.size() * sizeof(FVector4)
			+ TextureParameters.size() * (sizeof(FRHITextureReferenceRef) + sizeof(FMaterialSamplerState) + sizeof(EMaterialTextureFallback) + sizeof(EMaterialTextureDimension));
	}

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
		Properties.GeometryUsage &= uint8(~4u);
		if (Properties.ShadingModel == EMaterialShadingModel::SingleLayerWater)
		{
			if (Properties.PassUsage & (3u | 16u))
				Properties.PassUsage = (Properties.PassUsage & ~2u) | 1u | 16u;
		}
		else
		{
			Properties.RefractionMethod = EMaterialRefractionMethod::None;
		}
		if (Properties.RefractionMethod == EMaterialRefractionMethod::None || Properties.RefractionDepthBias == 0.0f)
			Properties.RefractionDepthBias = 0.0f;
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
			Texture->Texture = Cast<DTexture>(Object);
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

	auto FMaterialParameterValue::MakeTexture(DTexture* Value, FMaterialSamplerState Sampler,
		EMaterialTextureFallback Fallback) -> FMaterialParameterValue
	{
		FMaterialParameterValue Result;
		Result.Value = FMaterialTextureValue{Value, Sampler, Fallback, Cast<DTextureCube>(Value)
			? EMaterialTextureDimension::TextureCube : EMaterialTextureDimension::Texture2D};
		return Result;
	}

	auto FMaterialParameterValue::MakeTextureCube(DTextureCube* Value, FMaterialSamplerState Sampler,
		EMaterialTextureFallback Fallback) -> FMaterialParameterValue
	{
		auto Result = MakeTexture(Value, Sampler, Fallback);
		Result.GetTexture().Dimension = EMaterialTextureDimension::TextureCube;
		return Result;
	}

	auto FMaterialTextureValue::IsValid() const -> bool
	{
		if (!IsValidMaterialSampling(SamplerState, TextureFallback) || Dimension > EMaterialTextureDimension::TextureCube)
			return false;
		if (Dimension == EMaterialTextureDimension::TextureCube && TextureFallback == EMaterialTextureFallback::FlatRGNormal)
			return false;
		if (!Texture) return true;
		return Dimension == EMaterialTextureDimension::TextureCube
			? Cast<DTextureCube>(Texture.Get()) != nullptr : Cast<DTexture2D>(Texture.Get()) != nullptr;
	}

	auto IsValidMaterialSampling(FMaterialSamplerState State, EMaterialTextureFallback Fallback) -> bool
	{
		return State.MinFilter <= EMaterialSamplerMinFilter::LinearMipmapLinear
			&& State.MagFilter <= EMaterialSamplerMagFilter::Linear
			&& State.AddressU <= EMaterialSamplerAddressMode::ClampToEdge
			&& State.AddressV <= EMaterialSamplerAddressMode::ClampToEdge
			&& State.AddressW <= EMaterialSamplerAddressMode::ClampToEdge
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
			if (!Private::IsAuthoringMaterialParameterValue(Definition.Value))
				return {EMaterialParameterError::InvalidDefault, Definition.Id};
			if (Definition.Type != Definition.Value.GetType())
				return {EMaterialParameterError::InvalidType, Definition.Id};
			if (Definition.Type == EMaterialParameterType::Texture
				&& (!Definition.Value.GetTexture().IsValid()
					|| (Definition.Value.GetTexture().Dimension == EMaterialTextureDimension::TextureCube && Definition.TextureUsage == ETextureUsage::Normal)))
				return {EMaterialParameterError::InvalidMetadata, Definition.Id};
			if (!Private::IsFiniteMaterialParameterValue(Definition.Value))
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
		const FMaterialStaticProperties& Properties) -> std::expected<void, std::string>
	{
		// Built-in authored usage remains valid before renderer module registration.
		uint8 GeometryMask = 7;
		for (const auto& D : GetMaterialShaderEntries()) if (D.VertexFactory) GeometryMask |= D.GeometryUsage;
		if (!Properties.GeometryUsage || (Properties.GeometryUsage & ~GeometryMask)
			|| !Properties.PassUsage || (Properties.PassUsage & ~(Properties.ShadingModel == EMaterialShadingModel::SingleLayerWater ? 31u : 15u))
			|| ((Properties.GeometryUsage & 4) && !(Properties.GeometryUsage & 1)))
			return std::unexpected("Material geometry/pass usage masks are invalid.");
		switch (Properties.BlendMode)
		{
		case EMaterialBlendMode::Opaque:
		case EMaterialBlendMode::Masked:
		case EMaterialBlendMode::Translucent:
			break;
		default:
			return std::unexpected("Material blend mode is invalid.");
		}
		switch (Properties.ShadingModel)
		{
		case EMaterialShadingModel::DefaultLit:
		case EMaterialShadingModel::Unlit:
		case EMaterialShadingModel::ClearCoat:
		case EMaterialShadingModel::SingleLayerWater:
			break;
		default:
			return std::unexpected("Material shading model is invalid.");
		}
		if (Properties.ShadingModel == EMaterialShadingModel::ClearCoat && Properties.BlendMode == EMaterialBlendMode::Translucent)
			return std::unexpected("Translucent ClearCoat is unsupported.");
		if (Properties.ShadingModel == EMaterialShadingModel::SingleLayerWater && Properties.BlendMode == EMaterialBlendMode::Translucent)
			return std::unexpected("Single Layer Water requires Opaque or Masked blending.");
		if (Properties.RefractionMethod > EMaterialRefractionMethod::PixelNormalOffset
			|| !std::isfinite(Properties.RefractionDepthBias))
			return std::unexpected("Material refraction settings are invalid.");
		switch (Properties.DepthWritePolicy)
		{
		case EMaterialDepthWritePolicy::Automatic:
		case EMaterialDepthWritePolicy::Enabled:
		case EMaterialDepthWritePolicy::Disabled:
			break;
		default:
			return std::unexpected("Material depth-write policy is invalid.");
		}
		if (!std::isfinite(Properties.OpacityMaskThreshold)
			|| Properties.OpacityMaskThreshold < 0.0f
			|| Properties.OpacityMaskThreshold > 1.0f)
		{
			return std::unexpected("Material opacity-mask threshold must be finite and in the inclusive range [0, 1].");
		}
		return {};
	}
    auto ValidateMaterialConfiguration(EMaterialDomain Domain, const FMaterialStaticProperties& Properties) -> std::expected<void, std::string>
    {
        if (Domain > EMaterialDomain::PostProcess) return std::unexpected("Unsupported material domain.");
        if (const auto Valid = ValidateMaterialStaticProperties(Properties); !Valid) return Valid;
        if (Domain == EMaterialDomain::PostProcess && (Properties.ShadingModel != EMaterialShadingModel::Unlit || Properties.BlendMode != EMaterialBlendMode::Opaque))
            return std::unexpected("PostProcess requires opaque Unlit evaluation.");
        return {};
    }

}
