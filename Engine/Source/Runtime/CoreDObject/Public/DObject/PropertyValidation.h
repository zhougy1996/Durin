#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"

namespace Durin
{
	class FProperty;
	enum class EPropertyValidationError : uint8
	{
		InvalidType, InvalidStorage, MissingReferencedType, MissingValueOperations,
		InvalidContainerOperations, MissingContainerField,
	};
	struct FPropertyValidationError
	{
		EPropertyValidationError Code;
		std::string PropertyPath;
		std::string Reason;
	};
	struct FNativePropertyValidationOptions
	{
		// Ordinary struct fields can be constructed before native struct ops resolve.
		bool RequireValueOperations = false;
	};
	// Validates one concrete native field. Tree/role/layout policies belong to callers.
	COREDOBJECT_API auto ValidateNativeProperty(const FProperty& Property,
		FNativePropertyValidationOptions Options = {}) -> std::optional<FPropertyValidationError>;
}
