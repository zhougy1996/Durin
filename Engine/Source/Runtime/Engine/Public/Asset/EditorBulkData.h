#pragma once

#include "EngineAPI.h"
#include "Asset/PackageResource.h"
#include "Serialization/Archive.h"

namespace Durin::Asset
{
	inline constexpr uint32 EditorBulkDataContentIdVersion = 1;
	inline constexpr uint64 MaximumAuthoredBulkBytes = 1024ull * 1024ull * 1024ull;

	struct FEditorBulkDataSource
	{
		FPackageResourceHandle Resource;
		uint64 SegmentOffset = 0;
		uint64 StoredSize = 0;
		uint32 StorageFlags = 0;
		uint32 Alignment = 1;
	};

	// Owns authored content identity and an immutable memory or package-resource snapshot.
	class FEditorBulkData
	{
	public:
		ENGINE_API FEditorBulkData();
		ENGINE_API explicit FEditorBulkData(FGuid InstanceId);

		auto GetInstanceId() const -> const FGuid& { return InstanceId; }
		auto GetPayloadId() const -> FXxHash128 { return ContentId; }
		auto GetPayloadSize() const -> uint64 { return LogicalSize; }
		auto IsMemoryResident() const -> bool { return bHasMemory; }
		ENGINE_API auto GetPayload() const -> FPackageResourceRequest;
		ENGINE_API auto UpdatePayload(std::span<const std::byte> Bytes) -> bool;
		ENGINE_API auto UpdatePayload(FSharedByteBuffer Buffer) -> bool;
		ENGINE_API static auto TryCreatePackageBacked(
			FGuid InstanceId,
			FXxHash128 ContentId,
			uint64 LogicalSize,
			FEditorBulkDataSource Source,
			FEditorBulkData& OutValue,
			std::string* OutError = nullptr) -> bool;

		// Transitional v6 reader/import adapter; authored callers migrate to UpdatePayload.
		ENGINE_API auto ReplaceBytes(std::span<const std::byte> Bytes) -> bool;
		ENGINE_API auto ReplaceBytes(FGuid LegacyInstanceId, std::span<const std::byte> Bytes) -> bool;

		ENGINE_API auto Serialize(FArchive& Ar) -> void;
		ENGINE_API auto Identical(const FEditorBulkData& Other) const -> bool;

	private:
		FGuid InstanceId;
		FXxHash128 ContentId;
		uint64 LogicalSize = 0;
		FSharedByteBuffer Memory;
		FEditorBulkDataSource Source;
		bool bHasMemory = true;
	};
}
