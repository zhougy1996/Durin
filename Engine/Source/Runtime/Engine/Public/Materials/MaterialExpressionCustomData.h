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
		DPROPERTY(Edit)
		uint32 DataIndex = 0;
		DPROPERTY(Edit)
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
