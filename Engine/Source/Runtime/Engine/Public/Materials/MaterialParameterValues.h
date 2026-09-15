#pragma once

#include "Materials/MaterialTypes.h"

#include "MaterialParameterValues.gen.h"

namespace Durin
{
	// Persists only the scalar value selected by this record's type.
	DSTRUCT()
	struct FMaterialScalarParameterValue
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		float Value = 0.0f;

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Scalar;
		static auto SupportsType(EMaterialParameterType InType) -> bool { return InType == Type; }
		static auto PropertyName() -> FName { return FName("ScalarParameterValues"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue::MakeScalar(Value); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = InValue.GetScalar(); }
	};

	// Stores the four components of a vector parameter.
	DSTRUCT()
	struct FMaterialVectorParameterValue
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		FVector4f Value{0.0f};

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Vector4;
		static auto SupportsType(EMaterialParameterType InType) -> bool { return InType == Type; }
		static auto PropertyName() -> FName { return FName("VectorParameterValues"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue::MakeVector4(FVector4(Value)); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = FVector4f(InValue.GetVector4()); }

	};

	// Persists only the texture value selected by this record's type.
	DSTRUCT()
	struct FMaterialTextureParameterValue
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		FMaterialTextureValue Value = {};

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Texture;
		static auto SupportsType(EMaterialParameterType InType) -> bool { return InType == Type; }
		static auto PropertyName() -> FName { return FName("TextureParameterValues"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue::MakeTexture(Value.Texture.Get(), Value.SamplerState, Value.TextureFallback); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = {InValue.GetTexture().Texture, InValue.GetTexture().SamplerState, InValue.GetTexture().TextureFallback}; }
	};

	// Dispatches reflected scratch edits to the concrete record type.
	template<typename TVisitor>
	auto VisitMaterialParameterValueType(EMaterialParameterType Type, TVisitor&& Visitor) -> bool
	{
		switch (Type)
		{
		case EMaterialParameterType::Scalar: return Visitor.template operator()<FMaterialScalarParameterValue>();
		case EMaterialParameterType::Vector4: return Visitor.template operator()<FMaterialVectorParameterValue>();
		case EMaterialParameterType::Texture: return Visitor.template operator()<FMaterialTextureParameterValue>();
		}
		return false;
	}
}
