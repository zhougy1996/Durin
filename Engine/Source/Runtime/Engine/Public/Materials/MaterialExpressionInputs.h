#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin
{
	inline auto FindMaterialNumericInput(const DMaterialExpression& Expression,
		const FMaterialExpressionInput& Input) -> const FMaterialNumericInput*
	{
		const FMaterialNumericInput* Result = nullptr;
		Expression.GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct
				|| static_cast<FStructProperty*>(Property)->GetStruct() != FMaterialNumericInput::StaticStruct()) return;
			const auto* Numeric = static_cast<const FMaterialNumericInput*>(Property->GetValuePtr(&Expression));
			if (&Numeric->Connection == &Input) Result = Numeric;
		});
		return Result;
	}

	inline auto FindMaterialNumericInput(DMaterialExpression& Expression,
		const FMaterialExpressionInput& Input) -> FMaterialNumericInput*
	{
		return const_cast<FMaterialNumericInput*>(FindMaterialNumericInput(std::as_const(Expression), Input));
	}

	inline auto FindMaterialExpressionInputDefault(DMaterialExpression& Expression,
		const FMaterialExpressionInput& Input) -> std::vector<float>*
	{
		auto* Numeric = FindMaterialNumericInput(Expression, Input);
		return Numeric ? &Numeric->Constant : nullptr;
	}

	// The visitor borrows stored connection fields for the duration of the call.
	template<typename Visitor>
	auto VisitMaterialExpressionInputs(DMaterialExpression& Expression, Visitor&& Visit) -> void
	{
		Expression.VisitStoredInputs([&](uint32 Index, FMaterialExpressionInput& Input) { Visit(Index, Input); });
	}

	// Visit stored connections without materializing absent attribute defaults.
	template<typename Visitor>
	auto VisitStoredMaterialInputs(DMaterialExpression& Expression, Visitor&& Visit) -> void
	{
		Expression.VisitStoredInputs([&](uint32, FMaterialExpressionInput& Input) { Visit(Input); });
	}
}
