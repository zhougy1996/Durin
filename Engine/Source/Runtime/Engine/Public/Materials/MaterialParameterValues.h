#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialTypes.h"

#include "MaterialParameterValues.gen.h"

namespace Durin
{
	// Persists only the scalar value selected by this record's type.
	DSTRUCT()
	struct FMaterialScalarParameterValue
	{
		GENERATED_BODY()
		auto operator==(const FMaterialScalarParameterValue&) const -> bool = default;

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
		auto operator==(const FMaterialVectorParameterValue&) const -> bool = default;

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
		auto operator==(const FMaterialTextureParameterValue&) const -> bool = default;

		DPROPERTY()
		FGuid ParameterId;

		DPROPERTY()
		FMaterialTextureValue Value = {};

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Texture;
		static auto SupportsType(EMaterialParameterType InType) -> bool { return InType == Type; }
		static auto PropertyName() -> FName { return FName("TextureParameterValues"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue(Value); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = InValue.GetTexture(); }
	};

	// Classification matches instance storage. Vector2/3 remain available to native
	// compiled layouts; reflected material instances persist vectors as Vector4.
	struct FMaterialInstanceParameterSet
	{
		std::vector<FMaterialScalarParameterValue> ScalarParameters;
		std::vector<std::pair<FGuid, FVector2>> Vector2Parameters;
		std::vector<std::pair<FGuid, FVector3>> Vector3Parameters;
		std::vector<FMaterialVectorParameterValue> VectorParameters;
		std::vector<FMaterialTextureParameterValue> TextureParameters;
		ENGINE_API auto SetParameter(const FGuid& Id, const FMaterialParameterValue& Value) -> void;
		ENGINE_API auto GetParameterValue(const FGuid& Id, FMaterialParameterValue& OutValue) const -> bool;
		ENGINE_API auto ResolveTextureReferences() -> void;
		auto GetParameterCount() const -> size_t { return ScalarParameters.size() + Vector2Parameters.size() + Vector3Parameters.size() + VectorParameters.size() + TextureParameters.size(); }
		ENGINE_API auto GetPayloadBytes() const -> uint64;
		auto IsRenderThreadSet() const -> bool { return std::ranges::all_of(TextureParameters, [](const auto& Parameter) { return !Parameter.Value.Texture; }); }
		auto operator==(const FMaterialInstanceParameterSet&) const -> bool = default;
		template<typename TVisitor> auto VisitParameters(TVisitor&& Visitor) const -> void
		{
			for (const auto& Parameter : ScalarParameters) Visitor(Parameter.ParameterId, Parameter.GetValue());
			for (const auto& [Id, Value] : Vector2Parameters) Visitor(Id, FMaterialParameterValue(Value));
			for (const auto& [Id, Value] : Vector3Parameters) Visitor(Id, FMaterialParameterValue(Value));
			for (const auto& Parameter : VectorParameters) Visitor(Parameter.ParameterId, Parameter.GetValue());
			for (const auto& Parameter : TextureParameters) Visitor(Parameter.ParameterId, Parameter.GetValue());
		}
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
		case EMaterialParameterType::Vector:
		case EMaterialParameterType::Vector2:
			// Reflected parameter records support vectors through Vector4 only.
			return false;
		}
		return false;
	}
}
