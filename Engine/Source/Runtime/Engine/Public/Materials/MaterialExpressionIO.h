#pragma once

#include "CoreMinimal.h"

#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "DObject/ObjectPtr.h"
#include "EngineAPI.h"
#include "Misc/Guid.h"

#include "MaterialExpressionIO.gen.h"

namespace Durin
{
	class DMaterialExpression;
	enum class EMaterialDomain : uint8;
	enum class EMaterialProgramOpcode : uint8;
	enum class EMaterialProgramValueType : uint8;
	enum class EMaterialProperty : uint8;

	// Graph connections carry identity only; numeric defaults belong to their expression.
	DSTRUCT()
	struct FMaterialExpressionInput
	{
		GENERATED_BODY()

		DPROPERTY()
		TObjectPtr<DMaterialExpression> Expression;

		DPROPERTY()
		int32 OutputIndex = 0;

		FMaterialExpressionInput() = default;
		FMaterialExpressionInput(DMaterialExpression* Upstream, int32 Index = 0)
			: Expression(Upstream), OutputIndex(Index) {}
		// Resolve an editor or recipe endpoint against the source's output table.
		ENGINE_API FMaterialExpressionInput(DMaterialExpression* Upstream, int32 Index, FGuid NamedOutput);
		auto IsConnected() const -> bool { return Expression.IsValid(); }
		ENGINE_API auto GetExpressionId() const -> FGuid;
		ENGINE_API auto GetOutputId() const -> FGuid;
		auto Connect(int32 Index, DMaterialExpression* Upstream) -> void
		{
			Expression = Upstream;
			OutputIndex = Index;
		}
		auto operator==(const FMaterialExpressionInput&) const -> bool = default;
	};

	// Numeric pins retain an explicit value independently of their connection.
	DSTRUCT()
	struct FMaterialNumericInput
	{
		GENERATED_BODY()
		FMaterialNumericInput() = default;
		explicit FMaterialNumericInput(uint32 Width) : Constant(Width, 0.f) {}
		DPROPERTY()
		FMaterialExpressionInput Connection;
		DPROPERTY()
		bool UseConstant = false;
		DPROPERTY()
		std::vector<float> Constant{0.f};

		auto SetConstant(std::vector<float> Value) -> void { Constant = std::move(Value); UseConstant = true; }
		auto operator=(const FMaterialExpressionInput& Value) -> FMaterialNumericInput& { Connection = Value; return *this; }
		operator FMaterialExpressionInput&() { return Connection; }
		operator const FMaterialExpressionInput&() const { return Connection; }
		auto operator==(const FMaterialNumericInput&) const -> bool = default;
	};

	// Non-owning property read used by root projection and MIR.
	struct FMaterialNumericInputView
	{
		const FMaterialExpressionInput& Connection;
		bool UseConstant;
		std::span<const float> Constant;
		FMaterialNumericInputView(const FMaterialExpressionInput& Link, bool Enabled, std::span<const float> Value)
			: Connection(Link), UseConstant(Enabled), Constant(Value) {}
		FMaterialNumericInputView(const FMaterialNumericInput& Input)
			: FMaterialNumericInputView(Input.Connection, Input.UseConstant, Input.Constant) {}
		auto Capture() const -> FMaterialNumericInput
		{
			FMaterialNumericInput Result;
			Result.Connection = Connection;
			Result.UseConstant = UseConstant;
			Result.Constant.assign(Constant.begin(), Constant.end());
			return Result;
		}
		auto operator==(const FMaterialNumericInput& Other) const -> bool
		{
			return Connection == Other.Connection && UseConstant == Other.UseConstant
				&& std::ranges::equal(Constant, Other.Constant);
		}
	};

	DSTRUCT()
	struct FScalarMaterialInput
	{
		GENERATED_BODY()
		DPROPERTY()
		FMaterialExpressionInput Connection;
		DPROPERTY()
		bool UseConstant = false;
		DPROPERTY()
		float Constant = 0.f;
	};

	DSTRUCT()
	struct FColorMaterialInput
	{
		GENERATED_BODY()
		DPROPERTY()
		FMaterialExpressionInput Connection;
		DPROPERTY()
		bool UseConstant = false;
		DPROPERTY()
		float Constant[3]{};
	};

	DSTRUCT()
	struct FVectorMaterialInput
	{
		GENERATED_BODY()
		DPROPERTY()
		FMaterialExpressionInput Connection;
		DPROPERTY()
		bool UseConstant = false;
		DPROPERTY()
		float Constant[3]{};
	};

	ENGINE_API auto GetMaterialNumericInputFallback(EMaterialProgramOpcode Opcode,
		EMaterialProgramValueType Type, uint32 Slot) -> std::vector<float>;

	struct FMaterialInputDescription
	{
		ENGINE_API explicit FMaterialInputDescription(const FMaterialExpressionInput& Packed);
		ENGINE_API FMaterialInputDescription(EMaterialProperty Property, FMaterialNumericInputView Input);
		const FMaterialExpressionInput* Connection = nullptr;
		std::optional<FMaterialNumericInputView> Numeric;
		EMaterialProgramValueType Type{};
		std::string_view Name;
	};

	// Detached editable values for bulk graph operations.
	struct FMaterialPropertyInputs
	{
		FMaterialNumericInput BaseColor{3};
		FMaterialNumericInput Normal{3};
		FMaterialNumericInput Metallic;
		FMaterialNumericInput Roughness;
		FMaterialNumericInput AmbientOcclusion;
		FMaterialNumericInput EmissiveColor{3};
		FMaterialNumericInput Opacity;
		FMaterialNumericInput OpacityMask;
		FMaterialNumericInput WorldPositionOffset{3};
		FMaterialNumericInput Specular;
		FMaterialNumericInput ClearCoat;
		FMaterialNumericInput ClearCoatRoughness;

		ENGINE_API auto Find(EMaterialProperty Property) const -> const FMaterialNumericInput*;
		auto Find(EMaterialProperty Property) -> FMaterialNumericInput*
			{ return const_cast<FMaterialNumericInput*>(std::as_const(*this).Find(Property)); }
		auto operator==(const FMaterialPropertyInputs&) const -> bool = default;
	};

	// Owned root state for detached graph construction, editing and comparison.
	struct FMaterialRootInputs
	{
		FMaterialPropertyInputs Properties;
		FMaterialExpressionInput Attributes;
		bool bUseMaterialAttributes = false;

		ENGINE_API auto GetExpressionInputDescription(EMaterialProperty Property) const -> std::optional<FMaterialInputDescription>;
		ENGINE_API auto FindInvalidConstantWidth() const -> std::optional<EMaterialProperty>;
		auto UsesMaterialAttributes() const -> bool { return bUseMaterialAttributes; }
		auto GetNumericInputForProperty(EMaterialProperty Property) const -> const FMaterialNumericInput* { return Properties.Find(Property); }
		ENGINE_API auto GetExpressionInputForProperty(EMaterialProperty Property) const -> const FMaterialExpressionInput*;
		auto GetExpressionInputForProperty(EMaterialProperty Property) -> FMaterialExpressionInput*
			{ return const_cast<FMaterialExpressionInput*>(std::as_const(*this).GetExpressionInputForProperty(Property)); }
		auto Get(EMaterialProperty Property) const -> const FMaterialNumericInput&
		{
			const auto* Input = Properties.Find(Property);
			check(Input);
			return *Input;
		}
		auto Get(EMaterialProperty Property) -> FMaterialNumericInput& { return const_cast<FMaterialNumericInput&>(std::as_const(*this).Get(Property)); }
		auto operator==(const FMaterialRootInputs&) const -> bool = default;
	};

	// Historical comparisons use authored identities, independent of managed object lifetime.
	struct FMaterialOutputBindings
	{
		struct FPropertyBinding
		{
			FGuid Source;
			int32 OutputIndex;
			bool UseConstant;
			std::vector<float> Constant;
			auto operator==(const FPropertyBinding&) const -> bool = default;
		};
		bool UseMaterialAttributes = false;
		FGuid AttributeSource;
		int32 AttributeOutputIndex = 0;
		std::vector<FPropertyBinding> Properties;
		auto operator==(const FMaterialOutputBindings&) const -> bool = default;
	};
	ENGINE_API auto CaptureMaterialOutputBindings(const FMaterialRootInputs& Root) -> FMaterialOutputBindings;
#if DURIN_WITH_EDITORONLY_DATA
	class DMaterial;
	ENGINE_API auto CaptureMaterialOutputBindings(const DMaterial& Material) -> FMaterialOutputBindings;
#endif

	ENGINE_API auto GetMaterialDomainInputProperties(EMaterialDomain Domain) -> std::span<const EMaterialProperty>;

	ENGINE_API auto SupportsMaterialRootConstant(EMaterialProperty Property) -> bool;
	ENGINE_API auto ReadMaterialNumericDefault(FMaterialNumericInputView Input, EMaterialProperty Property) -> std::vector<float>;
	ENGINE_API auto ReadMaterialOutputDefault(const FMaterialRootInputs& Outputs, EMaterialProperty Pin) -> std::vector<float>;
}
