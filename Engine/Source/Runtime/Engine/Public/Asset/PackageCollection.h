#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/AssetPath.h"
#include "EngineAPI.h"

namespace Durin
{
	class DPackage;
	struct FPackageCollectionResult
	{
		std::expected<void, Durin::UnifiedError::FError> Admission;
		size_t CollectedCount = 0;
		std::vector<FPackagePath> ReferencedPackages;
	};

	// Owner-thread admission, also rechecked by collection before touching flags.
	ENGINE_API auto CheckPackageCollectionAllowed() -> std::expected<void, Durin::UnifiedError::FError>;
	// Weak identities must still denote the resident instance. Stale/null entries
	// are ignored; duplicates collect once. No dirty-state or editor policy here.
	// Count records candidate packages actually removed, not surviving exports.
	ENGINE_API auto CollectPackageExports(std::span<const TWeakObjectPtr<DPackage>> Candidates)
		-> FPackageCollectionResult;
}
