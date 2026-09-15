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

	// Stores all vector widths as float components while preserving the declared parameter type.
	DSTRUCT()
	struct FMaterialVectorParameterValue
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		FVector4f Value{0.0f};

		DPROPERTY()
		EMaterialParameterType ParameterType = EMaterialParameterType::Vector4;

		static auto SupportsType(EMaterialParameterType Type) -> bool
		{
			return Type == EMaterialParameterType::Vector2 || Type == EMaterialParameterType::Vector
				|| Type == EMaterialParameterType::Vector4;
		}
		static auto PropertyName() -> FName { return FName("VectorParameterValues"); }
		auto GetValue() const -> FMaterialParameterValue
		{
			require(SupportsType(ParameterType));
			switch (ParameterType)
			{
			case EMaterialParameterType::Vector2: return FMaterialParameterValue::MakeVector2(FVector2(Value));
			case EMaterialParameterType::Vector: return FMaterialParameterValue::MakeVector(FVector3(Value));
			default: return FMaterialParameterValue::MakeVector4(FVector4(Value));
			}
		}
		auto SetValue(const FMaterialParameterValue& InValue) -> void
		{
			require(SupportsType(InValue.GetType()));
			ParameterType = InValue.GetType();
			switch (ParameterType)
			{
			case EMaterialParameterType::Vector2: Value = FVector4f(InValue.GetVector2(), 0.0f, 0.0f); break;
			case EMaterialParameterType::Vector: Value = FVector4f(InValue.GetVector(), 0.0f); break;
			default: Value = FVector4f(InValue.GetVector4()); break;
			}
		}
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
		case EMaterialParameterType::Vector2:
		case EMaterialParameterType::Vector:
		case EMaterialParameterType::Vector4: return Visitor.template operator()<FMaterialVectorParameterValue>();
		case EMaterialParameterType::Texture: return Visitor.template operator()<FMaterialTextureParameterValue>();
		}
		return false;
	}
}
