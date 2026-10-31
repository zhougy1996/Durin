#pragma once

#include "CoreMinimal.h"
#include "DObject/AuthoredOverridePath.h"

namespace Durin
{
	// Supported authored operations; ordinary differences and structural ancestors are None.
	enum class EOverriddenPropertyOperation : uint8 { None, Replace };
	struct FOverriddenPropertyQuery
	{
		EOverriddenPropertyOperation Operation = EOverriddenPropertyOperation::None;
		bool bInherited = false;
		bool bHasOverriddenDescendants = false;
		uint32 ReplacementDepth = 0;
		std::optional<EAuthoredOverrideProvenance> Provenance;
	};

	// Immutable, pointer-free replacement boundaries. Readers may retain a snapshot.
	class FOverriddenPropertySet final
	{
	public:
		auto GetEntries() const -> const std::vector<FAuthoredOverrideEntry>& { return Entries; }
		// Snapshot query compares canonical identities; live-route validation belongs to the manager.
		COREDOBJECT_API auto GetOverriddenPropertyOperation(const FAuthoredOverridePath& Path) const -> FOverriddenPropertyQuery;
	private:
		std::vector<FAuthoredOverrideEntry> Entries;
		friend class FOverridableManager;
	};
}
