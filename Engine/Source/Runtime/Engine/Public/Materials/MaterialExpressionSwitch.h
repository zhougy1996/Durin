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
		DPROPERTY()
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		DPROPERTY()
		FMaterialNumericInput DefaultValue;
		DPROPERTY()
		FMaterialExpressionInput Low;
		DPROPERTY()
		FMaterialExpressionInput High;
		auto GetAuthoredInputCount() const -> uint32 override { return 3; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionFeatureLevelSwitch : public DMaterialExpressionNumeric
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFeatureLevelSwitch(const FObjectInitializer& Initializer) : Super(Initializer) {}
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
		auto GetAuthoredInputCount() const -> uint32 override { return 4; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
