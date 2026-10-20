#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	// Capture-time identity owns path text; it never resolves by mutable names.
	struct FTransactionObjectId
	{
		FObjectKey OriginalObject;
		FName OriginalName;
		std::string OriginalOuterPath;
		std::string OriginalClassPath;
		std::string OriginalPackagePath;
		auto operator==(const FTransactionObjectId&) const -> bool = default;
	};
	COREDOBJECT_API auto CaptureTransactionObjectIdentity(const DObject& Object) -> FTransactionObjectId;
}
