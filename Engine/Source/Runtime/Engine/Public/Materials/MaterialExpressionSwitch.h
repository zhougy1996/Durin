#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionNumeric.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionSwitch.gen.h"

namespace Durin
{
	// Declares one root-owned compile-time boolean. It never lowers to MIR.
	DCLASS()
	class DMaterialExpressionStaticBool : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionStaticBool(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::StaticBool, .Name = "Static Bool",
				.Description = "Declares a root-owned compile-time boolean keyed by stable GUID.",
				.Category = "Static Selection", .FixedOutputType = EMaterialProgramValueType::StaticBool};
		}
		DPROPERTY(Edit)
		FGuid DeclarationId;
		DPROPERTY(Edit)
		FName Name = "StaticBool";
		DPROPERTY(Edit)
		bool DefaultValue = false;
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionStaticSwitch : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionStaticSwitch(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::StaticSwitch, .Name = "Static Switch",
				.Description = "Selects exactly one branch from a static bool before normalized MIR.",
				.Category = "Static Selection"};
		}
		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			return GetNumericInputInfo(Index, {"Condition", "False", "True"});
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		DPROPERTY()
		FMaterialExpressionInput Condition;
		DPROPERTY()
		FMaterialNumericInput FalseValue;
		DPROPERTY()
		FMaterialNumericInput TrueValue;
		auto GetAuthoredInputCount() const -> uint32 override { return 3; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionQualitySwitch : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionQualitySwitch(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::QualitySwitch, .Name = "Quality Switch",
				.Description = "Selects Low or High, using Default when that branch is unconnected.",
				.Category = "Static Selection"};
		}
		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			return GetNumericInputInfo(Index, {"Default", "Low", "High"});
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		DPROPERTY()
		FMaterialNumericInput DefaultValue;
		DPROPERTY()
		FMaterialExpressionInput Low;
		DPROPERTY()
		FMaterialExpressionInput High;
#if DURIN_WITH_EDITORONLY_DATA
		auto IsInputConnectionRequired(int32 Index) const -> bool override
		{
			return Index == 0 && Super::IsInputConnectionRequired(Index);
		}
#endif
		auto GetAuthoredInputCount() const -> uint32 override { return 3; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionFeatureLevelSwitch : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFeatureLevelSwitch(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::FeatureLevelSwitch, .Name = "Feature Level Switch",
				.Description = "Selects the accepted RHI feature tier, using Default when unconnected.",
				.Category = "Static Selection"};
		}
		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			return GetNumericInputInfo(Index, {"Default", "ES3_1", "SM5", "SM6"});
		}
		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		DPROPERTY()
		FMaterialNumericInput DefaultValue;
		DPROPERTY()
		FMaterialExpressionInput ES3_1;
		DPROPERTY()
		FMaterialExpressionInput SM5;
		DPROPERTY()
		FMaterialExpressionInput SM6;
#if DURIN_WITH_EDITORONLY_DATA
		auto IsInputConnectionRequired(int32 Index) const -> bool override
		{
			return Index == 0 && Super::IsInputConnectionRequired(Index);
		}
#endif
		auto GetAuthoredInputCount() const -> uint32 override { return 4; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
