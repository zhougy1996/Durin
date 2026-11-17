#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialFunctionTypes.h"

#include "MaterialExpressionFunctionIO.gen.h"

namespace Durin
{
	// Owns the function port definition; the function signature is derived from terminals.
	DCLASS()
	class DMaterialExpressionFunctionInput : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFunctionInput(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialFunctionPort Port;

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};

	// Owns the function port definition; the function signature is derived from terminals.
	DCLASS()
	class DMaterialExpressionFunctionOutput : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFunctionOutput(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialFunctionPort Port;

		DPROPERTY()
		FMaterialExpressionInput Source;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
