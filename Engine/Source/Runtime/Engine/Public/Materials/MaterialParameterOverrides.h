#pragma once

#include "Materials/MaterialTypes.h"

#include "MaterialParameterOverrides.gen.h"

namespace Durin
{
	// Persists only the scalar value selected by this record's type.
	DSTRUCT()
	struct FMaterialScalarParameterOverride
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		float Value = 0.0f;

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Scalar;
		static auto PropertyName() -> FName { return FName("ScalarParameterOverrides"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue::MakeScalar(Value); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = InValue.GetScalar(); }
	};

	// Persists only the vector2 value selected by this record's type.
	DSTRUCT()
	struct FMaterialVector2ParameterOverride
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		FVector2 Value{0.0};

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Vector2;
		static auto PropertyName() -> FName { return FName("Vector2ParameterOverrides"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue::MakeVector2(Value); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = InValue.GetVector2(); }
	};

	// Persists only the vector value selected by this record's type.
	DSTRUCT()
	struct FMaterialVectorParameterOverride
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		FVector3 Value{0.0};

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Vector;
		static auto PropertyName() -> FName { return FName("VectorParameterOverrides"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue::MakeVector(Value); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = InValue.GetVector(); }
	};

	// Persists only the vector4 value selected by this record's type.
	DSTRUCT()
	struct FMaterialVector4ParameterOverride
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		FVector4 Value{0.0};

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Vector4;
		static auto PropertyName() -> FName { return FName("Vector4ParameterOverrides"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue::MakeVector4(Value); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = InValue.GetVector4(); }
	};

	// Persists only the texture value selected by this record's type.
	DSTRUCT()
	struct FMaterialTextureParameterOverride
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		FMaterialTextureValue Value = {};

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Texture;
		static auto PropertyName() -> FName { return FName("TextureParameterOverrides"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue::MakeTexture(Value.Texture.Get(), Value.SamplerState, Value.TextureFallback); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = {InValue.GetTexture().Texture, InValue.GetTexture().SamplerState, InValue.GetTexture().TextureFallback}; }
	};

	// Dispatches reflected scratch edits to the concrete record type.
	template<typename TVisitor>
	auto VisitMaterialParameterOverrideType(EMaterialParameterType Type, TVisitor&& Visitor) -> bool
	{
		switch (Type)
		{
		case EMaterialParameterType::Scalar: return Visitor.template operator()<FMaterialScalarParameterOverride>();
		case EMaterialParameterType::Vector2: return Visitor.template operator()<FMaterialVector2ParameterOverride>();
		case EMaterialParameterType::Vector: return Visitor.template operator()<FMaterialVectorParameterOverride>();
		case EMaterialParameterType::Vector4: return Visitor.template operator()<FMaterialVector4ParameterOverride>();
		case EMaterialParameterType::Texture: return Visitor.template operator()<FMaterialTextureParameterOverride>();
		}
		return false;
	}
}
