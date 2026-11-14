#pragma once
#include "CoreMinimal.h"

namespace Durin
{
	// Paths describe committed effects, not attempted operations. Empty paths denote absence.
	enum class EContentChangeKind : uint8 { Added, Modified, Removed, Renamed };
	struct FContentChange
	{
		EContentChangeKind Kind = EContentChangeKind::Modified;
		std::string OldPhysicalPath;
		std::string NewPhysicalPath;
		std::string OldAssetPath;
		std::string NewAssetPath;
		bool bDirectory = false;
	};

	// Ordered changes spanning (FromRevision, ToRevision]; unknown coverage requires a rescan.
	struct FContentChangeBatch
	{
		uint64 FromRevision = 0;
		uint64 ToRevision = 0;
		bool bFullRefresh = false;
		std::vector<FContentChange> Changes;
	};

}
