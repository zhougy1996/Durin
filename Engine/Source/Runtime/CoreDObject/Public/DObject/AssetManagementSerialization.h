#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"

namespace Durin
{
	class FBinaryReader;
	class FBinaryWriter;
	struct FPrimaryAssetId;
	struct FAssetBundleData;

	// Versioned package/cache projection; the value types have no wire dependency.
	COREDOBJECT_API auto WriteAssetManagementMetadata(FBinaryWriter& Writer,
		const FPrimaryAssetId& Id, const FAssetBundleData& Data) -> void;
	COREDOBJECT_API auto ReadAssetManagementMetadata(FBinaryReader& Reader,
		FPrimaryAssetId& Id, FAssetBundleData& Data) -> bool;
}
