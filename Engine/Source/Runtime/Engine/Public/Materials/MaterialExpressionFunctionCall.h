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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::FunctionCall, .Name = "Function Call",
				.Description = "Evaluates a material function with its bound inputs.", .Category = "Functions",
				.bRequiresAsset = true, .bHasInstanceShape = true};
		}

		ENGINE_API auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override;
#if DURIN_WITH_EDITORONLY_DATA
		using Super::GetInput;
		ENGINE_API auto GetInput(int32 Index) -> FMaterialExpressionInput* override;
		auto IsInputConnectionRequired(int32 Index) const -> bool override
		{
			const auto* Port = GetInputInfo(Index).FunctionPort;
			return Port && Port->bRequired;
		}
		ENGINE_API auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override;
		ENGINE_API auto FindInputInfo(FGuid PortId) const -> FMaterialExpressionInputInfo;
		ENGINE_API auto GetInputPorts() const -> std::span<const FMaterialFunctionPort>;
		ENGINE_API auto GetOutputPorts() const -> std::span<const FMaterialFunctionPort>;
		ENGINE_API auto FindInputBindingIndex(FGuid PortId) const -> int32;
		ENGINE_API auto FindOutputBindingIndex(FGuid PortId) const -> int32;

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
		ENGINE_API auto VisitStoredInputs(const FStoredInputVisitor& Visit) -> void override;
		auto GetAuthoredInputCount() const -> uint32 override { return static_cast<uint32>(FunctionInputs.size()); }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
