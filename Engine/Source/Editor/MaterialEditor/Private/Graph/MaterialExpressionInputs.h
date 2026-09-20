#pragma once
#include "Materials/MaterialExpressions.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin::Editor::Material
{
	inline auto FindMaterialNumericInput(DMaterialExpression& Expression,
		const FMaterialExpressionInput& Input) -> FMaterialNumericInput*
	{
		if (auto* Surface = Cast<DMaterialExpressionSetSurfaceAttributes>(&Expression))
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
		if (auto* Call = Cast<DMaterialExpressionFunctionCall>(&Expression))
		{
			for (uint32 Index = 0; Index < Call->Inputs.size(); ++Index) Visit(Index, Call->Inputs[Index].Input);
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
		if (auto* Surface = Cast<DMaterialExpressionSetSurfaceAttributes>(&Expression))
			for (auto& Attribute : Surface->Attributes) Visit(static_cast<uint32>(Attribute.Attribute) + 1, Attribute.Source.Connection);
	}
}
