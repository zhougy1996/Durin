#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionCustomData.gen.h"

namespace Durin
{
	DCLASS()
	class DMaterialExpressionPrimitiveCustomData : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionPrimitiveCustomData(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::PrimitiveCustomData, .Name = "Custom Primitive Data",
				.Description = "Reads indexed numeric channels shared by the primitive's instances.",
				.Category = "Inputs", .SearchKeywords = "CPD",
				.bRegisterForEditorCreation = false};
		}

		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY(Edit)
		uint32 DataIndex = 0;
		DPROPERTY(Edit)
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionInstanceCustomData : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionInstanceCustomData(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::InstanceCustomData, .Name = "Per Instance Custom Data",
				.Description = "Reads indexed numeric channels of the current stable mesh instance.",
				.Category = "Inputs", .SearchKeywords = "PICD",
				.bRegisterForEditorCreation = false};
		}

		auto GetAuthoredResultType() const -> const EMaterialProgramValueType* override { return &ResultType; }
		DPROPERTY(Edit)
		uint32 DataIndex = 0;
		DPROPERTY(Edit)
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
