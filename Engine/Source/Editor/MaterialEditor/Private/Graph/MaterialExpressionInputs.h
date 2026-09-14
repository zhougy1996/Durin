#pragma once
#include "Materials/MaterialExpressions.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin::Editor::Material
{
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
		uint32 Index = 0;
		Expression.GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct) return;
			if (static_cast<FStructProperty*>(Property)->GetStruct() != FMaterialExpressionInput::StaticStruct()) return;
			for (uint32 Element = 0; Element < Property->GetArrayDim(); ++Element)
				Visit(Index++, *static_cast<FMaterialExpressionInput*>(Property->GetValuePtr(&Expression, Element)));
		});
		if (auto* Surface = Cast<DMaterialExpressionSetSurfaceAttributes>(&Expression))
			for (auto& Attribute : Surface->Attributes) Visit(static_cast<uint32>(Attribute.Attribute) + 1, Attribute.Source);
	}
}
