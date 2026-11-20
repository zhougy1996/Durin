#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialOutput.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin
{
	inline auto FindMaterialNumericInput(DMaterialExpression& Expression,
		const FMaterialExpressionInput& Input) -> FMaterialNumericInput*
	{
		if (auto* Attributes = Cast<DMaterialExpressionMakeMaterialAttributes>(&Expression))
			for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
			{ auto& Numeric = Attributes->Values.Get(Definition.Property); if (&Numeric.Connection == &Input) return &Numeric; }

		if (auto* Surface = Cast<DMaterialExpressionSetMaterialAttributes>(&Expression))
			for (auto& Attribute : Surface->Attributes) if (&Attribute.Source.Connection == &Input) return &Attribute.Source;
		FMaterialNumericInput* Result = nullptr;
		Expression.GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct
				|| static_cast<FStructProperty*>(Property)->GetStruct() != FMaterialNumericInput::StaticStruct()) return;
			auto* Numeric = static_cast<FMaterialNumericInput*>(Property->GetValuePtr(&Expression));
			if (&Numeric->Connection == &Input) Result = Numeric;
		});
		return Result;
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
				Visit(uint32(Definition.Property), Attributes->Values.Get(Definition.Property).Connection);
			return;
		}

		if (auto* Call = Cast<DMaterialExpressionFunctionCall>(&Expression))
		{
			for (uint32 Index = 0; Index < Call->FunctionInputs.size(); ++Index) Visit(Index, Call->FunctionInputs[Index].Input);
			return;
		}
		if (auto* Output = Cast<DMaterialExpressionMaterialOutput>(&Expression))
		{
			for (const auto& Pin : GetMaterialDomainOutputPins(EMaterialDomain::Surface))
				Visit(static_cast<uint32>(Pin.Id), *GetMaterialOutputInput(Output->Outputs, Pin.Id));
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
			for (auto& Attribute : Surface->Attributes) Visit(static_cast<uint32>(Attribute.GetProperty()) + 1, Attribute.Source.Connection);
	}

	// Visit stored connections without materializing absent attribute defaults.
	template<typename Visitor>
	auto VisitStoredMaterialInputs(DMaterialExpression& Expression, Visitor&& Visit) -> void
	{
		if (auto* Output = Cast<DMaterialExpressionMaterialOutput>(&Expression))
		{
			Visit(Output->Outputs.Attributes);
			for (auto& Binding : Output->Outputs.Properties.Bindings) Visit(Binding.Input.Connection);
		}
		else if (auto* Make = Cast<DMaterialExpressionMakeMaterialAttributes>(&Expression))
		{
			Visit(Make->Values.Attributes);
			for (auto& Binding : Make->Values.Properties.Bindings) Visit(Binding.Input.Connection);
		}
		else VisitMaterialExpressionInputs(Expression, [&](uint32, FMaterialExpressionInput& Input) { Visit(Input); });
	}
}
