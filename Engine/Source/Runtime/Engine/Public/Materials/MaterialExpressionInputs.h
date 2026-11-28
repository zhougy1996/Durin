#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
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

	// Reflected connection members retain declaration order, matching fixed pins.
	// Dynamic surface pins use attribute indices; calls use their binding order.
	template<typename Visitor>
	auto VisitMaterialExpressionInputs(DMaterialExpression& Expression, Visitor&& Visit) -> void
	{
		if (auto* Attributes = Cast<DMaterialExpressionMakeMaterialAttributes>(&Expression))
		{
			for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
				Visit(uint32(Definition.Property), *Attributes->GetExpressionInputForProperty(Definition.Property));
			return;
		}

		if (auto* Call = Cast<DMaterialExpressionFunctionCall>(&Expression))
		{
			for (uint32 Index = 0; Index < Call->FunctionInputs.size(); ++Index) Visit(Index, Call->FunctionInputs[Index].Input);
			return;
		}
		uint32 Index = 0;
		Expression.GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct) return;
			if (static_cast<FStructProperty*>(Property)->GetStruct() == FMaterialNumericInput::StaticStruct())
			{
				for (uint32 Element = 0; Element < Property->GetArrayDim(); ++Element)
					Visit(Index++, static_cast<FMaterialNumericInput*>(Property->GetValuePtr(&Expression, Element))->Connection);
				return;
			}
			if (static_cast<FStructProperty*>(Property)->GetStruct() != FMaterialExpressionInput::StaticStruct()) return;
			for (uint32 Element = 0; Element < Property->GetArrayDim(); ++Element)
				Visit(Index++, *static_cast<FMaterialExpressionInput*>(Property->GetValuePtr(&Expression, Element)));
		});
		if (auto* Surface = Cast<DMaterialExpressionSetMaterialAttributes>(&Expression))
			for (auto& Attribute : Surface->Attributes) Visit(static_cast<uint32>(Attribute.GetProperty()) + 1, Attribute.Input);
	}

	// Visit stored connections without materializing absent attribute defaults.
	template<typename Visitor>
	auto VisitStoredMaterialInputs(DMaterialExpression& Expression, Visitor&& Visit) -> void
	{
		VisitMaterialExpressionInputs(Expression, [&](uint32, FMaterialExpressionInput& Input) { Visit(Input); });
	}
}
