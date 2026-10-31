#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectKey.h"
#include "DObject/OverriddenPropertySet.h"

namespace Durin
{
	class FObjectArchive;
	struct FSerializedPropertyChainState;

	// Bounded diagnostic vocabulary; reasons never authorize bypassing strict validation.
	enum class EOverrideRestoreFallbackReason : uint8
	{
		None, FilteredContext, CustomObjectSerializer, CustomStructSerializer,
		PostDeserialize, IncompleteAuthoredFields, UnprovenMetadata, UnsupportedPath,
		ArchiveFailure, MetadataChanged, Count
	};

	// One successful traversal, one target, one publication. Only object archives build evidence.
	class FOverriddenPropertyRestoreContext final
	{
	public:
		FOverriddenPropertyRestoreContext(const FOverriddenPropertyRestoreContext&) = delete;
		auto operator=(const FOverriddenPropertyRestoreContext&) -> FOverriddenPropertyRestoreContext& = delete;
		auto GetFallbackReason() const -> EOverrideRestoreFallbackReason { return Reason; }
		auto GetPathVisits() const -> uint64 { return PathVisits; }
		COREDOBJECT_API auto IsEligible() const -> bool;
		auto GetCandidateCount() const -> uint64 { return Candidates.size(); }
	private:
		COREDOBJECT_API FOverriddenPropertyRestoreContext(DObject& Object, const FObjectArchive& Archive);
		FObjectKey Target;
		std::vector<std::pair<FName, FObjectKey>> Types;
		std::vector<FAuthoredOverrideEntry> Candidates;
		EOverrideRestoreFallbackReason Reason = EOverrideRestoreFallbackReason::None;
		uint64 PathVisits = 0;
		bool bSawObject = false;
		bool bComplete = false;
		std::weak_ptr<const FSerializedPropertyChainState> Traversal;
		uint64 TraversalRevision = 0;
		friend class FObjectArchive;
		friend class FOverridableManager;
	};
}
