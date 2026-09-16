#pragma once
#include "Materials/MaterialExpressions.h"

namespace Durin::Testing
{
	// Test builders may describe expected ports together; publication still consumes nodes only.
	inline auto WithFunctionPorts(const FMaterialFunctionSignature& Ports,
		std::span<DMaterialExpression* const> Expressions) -> std::vector<DMaterialExpression*>
	{
		for (auto* Expression : Expressions)
		{
			if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression))
				if (const auto Port = std::ranges::find(Ports.Inputs, Input->Port.Id, &FMaterialFunctionPort::Id); Port != Ports.Inputs.end()) Input->Port = *Port;
			if (auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression))
				if (const auto Port = std::ranges::find(Ports.Outputs, Output->Port.Id, &FMaterialFunctionPort::Id); Port != Ports.Outputs.end()) Output->Port = *Port;
		}
		return {Expressions.begin(), Expressions.end()};
	}
}
