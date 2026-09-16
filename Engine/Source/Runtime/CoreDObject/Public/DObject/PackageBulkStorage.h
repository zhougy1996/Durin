#pragma once

#include "CoreDObjectAPI.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"
#include "Serialization/SharedByteBuffer.h"

namespace Durin
{
	// Selects whether authored payload bytes accompany their descriptor in DAST or a local companion.
	enum class EPackageBulkStorageKind : uint8 { Inline, External };

	// Describes one authored payload inside a package publication transaction.
	struct FPackageBulkStorageDescriptor
	{
		FGuid PayloadId;
		uint64 LogicalByteCount = 0;
		uint64 StoredByteCount = 0;
		FXxHash128 ContentHash;
		FXxHash128 ContainerHash;
		EPackageBulkStorageKind StorageKind = EPackageBulkStorageKind::Inline;
		uint64 SegmentOffset = 0;
		uint32 Alignment = 1;

		auto operator==(const FPackageBulkStorageDescriptor&) const -> bool = default;
	};

	// Carries verified external authored bytes into package publication.
	struct FPackageBulkStoragePayload
	{
		FPackageBulkStorageDescriptor Descriptor;
		FSharedByteBuffer Buffer;
	};
}
