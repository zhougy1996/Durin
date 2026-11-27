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

		DPROPERTY(EditorOnly)
		FGuid ParameterId;

		DPROPERTY()
		float Value = 0.0f;

		DPROPERTY()
		FMaterialParameterInfo ParameterInfo;

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

		DPROPERTY(EditorOnly)
		FGuid ParameterId;

		DPROPERTY()
		FVector4f Value{0.0f};

		DPROPERTY()
		FMaterialParameterInfo ParameterInfo;

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

		DPROPERTY(EditorOnly)
		FGuid ParameterId;

		DPROPERTY()
		FMaterialTextureValue Value = {};

		DPROPERTY()
		FMaterialParameterInfo ParameterInfo;

		static constexpr EMaterialParameterType Type = EMaterialParameterType::Texture;
		static auto SupportsType(EMaterialParameterType InType) -> bool { return InType == Type; }
		static auto PropertyName() -> FName { return FName("TextureParameterValues"); }
		auto GetValue() const -> FMaterialParameterValue { return FMaterialParameterValue(Value); }
		auto SetValue(const FMaterialParameterValue& InValue) -> void { Value = InValue.GetTexture(); }
	};

	// Sparse values bound to one compiled layout, ordered by field index.
	struct FMaterialInstanceParameterSet
	{
		struct FEntry
		{
			uint32 FieldIndex;
			FMaterialParameterValue Value;
			auto operator==(const FEntry&) const -> bool = default;
		};
		auto GetFields() const -> std::span<const FEntry> { return Fields; }
		ENGINE_API auto SetParameter(uint32 FieldIndex, const FMaterialParameterValue& Value) -> void;
		ENGINE_API auto GetParameterValue(uint32 FieldIndex, FMaterialParameterValue& OutValue) const -> bool;
		ENGINE_API auto ResolveTextureReferences() -> void;
		auto GetParameterCount() const -> size_t { return Fields.size(); }
		ENGINE_API auto GetPayloadBytes() const -> uint64;
		auto IsRenderThreadSet() const -> bool { return std::ranges::all_of(Fields, [](const auto& Field) { return Field.Value.IsRenderThreadValue(); }); }
		auto operator==(const FMaterialInstanceParameterSet&) const -> bool = default;
		template<typename TVisitor> auto VisitParameters(TVisitor&& Visitor) const -> void
		{
			for (const auto& Field : Fields) Visitor(Field.FieldIndex, Field.Value);
		}
	private:
		std::vector<FEntry> Fields;
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
