#pragma once
#include "Materials/MaterialTypes.h"

namespace Durin
{
	inline auto ValidateNamedMaterialParameter(const FMaterialParameterDefinition* Definition,
		FName Name, EMaterialParameterType Type) -> FMaterialOperationResult
	{
		if (Definition && Definition->Type == Type) return {};
		FMaterialError Error(Definition ? EMaterialParameterError::InvalidType : EMaterialParameterError::NotFound);
		Error.ParameterName = Name.ToString();
		Error.ActualParameterType = Type;
		if (Definition)
		{
			Error.ParameterId = Definition->Id;
			Error.ExpectedParameterType = Definition->Type;
		}
		return {std::move(Error)};
	}

	inline auto WithMaterialParameterName(FMaterialOperationResult Result, FName Name) -> FMaterialOperationResult
	{
		if (!Result) Result.Error.ParameterName = Name.ToString();
		return Result;
	}
}
