#pragma once

#include "CoreMinimal.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class FProperty;
	class FStrongObjectPtr;
	struct FReferencerInformation
	{
		FWeakObjectPtr Referencer;
		int32 TotalReferences = 0;
		std::vector<const FProperty*> ReferencingProperties;
	};
	struct FReferencerInformationList
	{
		std::vector<FReferencerInformation> InternalReferences;
		std::vector<FReferencerInformation> ExternalReferences;
	};
	// Owner-thread, read-only traversal. Property descriptors require their owning
	// reflected module to remain loaded. Native owners have no synthetic properties.
	COREDOBJECT_API auto FindObjectReferencers(DObject* Object,
		std::span<DObject* const> DeletionRoots, FReferencerInformationList& Out,
		std::span<const FStrongObjectPtr* const> RetainedByModel = {}) -> bool;
}
