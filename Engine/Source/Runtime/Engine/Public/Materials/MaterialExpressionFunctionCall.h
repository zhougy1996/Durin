#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialFunctionTypes.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionFunctionCall.gen.h"

namespace Durin
{
	// A call binding retains a numeric default only when present.
	DSTRUCT()
	struct FMaterialExpressionFunctionInputBinding
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid InputId;

		DPROPERTY()
		EMaterialProgramValueType ExpectedType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialExpressionInput Input;

		DPROPERTY()
		std::vector<float> InputDefault;
		auto operator==(const FMaterialExpressionFunctionInputBinding&) const -> bool = default;
	};

	// Owns its callee and GUID-keyed port bindings inside the graph.
	DCLASS()
	class DMaterialExpressionFunctionCall : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionFunctionCall(const FObjectInitializer& Initializer) : Super(Initializer) {}

#if DURIN_WITH_EDITORONLY_DATA
		DPROPERTY(EditorOnly)
		TObjectPtr<DMaterialFunctionInterface> Function;
#endif

		DPROPERTY(LegacyNames = "Inputs")
		std::vector<FMaterialExpressionFunctionInputBinding> FunctionInputs;

		DPROPERTY(LegacyNames = "Outputs")
		std::vector<FMaterialFunctionOutputBinding> FunctionOutputs;

#if DURIN_WITH_EDITORONLY_DATA
		// Returns whether bindings changed. Load and batched updates publish their owner separately.
		ENGINE_API auto UpdateFromFunctionResource(bool bRecreateAndLinkNode = true) -> bool;
		ENGINE_API auto SetMaterialFunction(DMaterialFunctionInterface* NewFunction) -> bool;
#endif
		auto GetAuthoredInputCount() const -> uint32 override { return static_cast<uint32>(FunctionInputs.size()); }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
