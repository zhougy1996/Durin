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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::FunctionInput, .Name = "Function Input",
				.Description = "Reads an input from the function signature.", .Category = "Functions",
				.bRequiresAsset = true, .bHasInstanceShape = true};
		}
#if DURIN_WITH_EDITORONLY_DATA
		auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override
		{
			if (Index == 0) return FMaterialExpressionOutput{.Name = Port.Name, .Type = Port.Type};
			return {};
		}
		auto CanRenameNode() const -> bool override { return true; }
		auto GetEditableName() const -> std::string override { return Port.Name; }
		auto SetEditableName(std::string NewName) -> void override { Port.Name = std::move(NewName); }
#endif

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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::FunctionOutput, .Name = "Function Output",
				.Description = "Publishes a value through the function signature.", .Category = "Functions",
				.bRequiresAsset = true, .bHasInstanceShape = true};
		}
		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			if (Index == 0) return {.Name = Port.Name, .Type = Port.Type, .bAllowImplicitCast = true};
			return {};
		}
#if DURIN_WITH_EDITORONLY_DATA
		auto CanRenameNode() const -> bool override { return true; }
		auto GetEditableName() const -> std::string override { return Port.Name; }
		auto SetEditableName(std::string NewName) -> void override { Port.Name = std::move(NewName); }
#endif

		DPROPERTY()
		FMaterialFunctionPort Port;

		DPROPERTY()
		FMaterialExpressionInput Source;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
