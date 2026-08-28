#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"
#include "Serialization/SharedByteBuffer.h"

namespace Durin::Asset
{
	// Selects whether authored payload bytes accompany their descriptor in DAST or a local companion.
	enum class EEditorBulkDataStorageKind : uint8 { Inline, External };

	// Describes one authored payload inside a package publication transaction.
	struct FEditorBulkDataStorageDescriptor
	{
		FGuid PayloadId;
		uint64 LogicalByteCount = 0;
		uint64 StoredByteCount = 0;
		FXxHash128 ContentHash;
		FXxHash128 ContainerHash;
		EEditorBulkDataStorageKind StorageKind = EEditorBulkDataStorageKind::Inline;

		auto operator==(const FEditorBulkDataStorageDescriptor&) const -> bool = default;
	};

	// Carries verified external authored bytes into package publication.
	struct FEditorBulkDataStoragePayload
	{
		FEditorBulkDataStorageDescriptor Descriptor;
		FSharedByteBuffer Buffer;
	};
}
