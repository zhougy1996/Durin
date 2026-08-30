#pragma once

#include "EngineAPI.h"
#include "Asset/PackageResource.h"
#include "Serialization/Archive.h"

namespace Durin::Asset
{
	inline constexpr uint32 EditorBulkDataContentIdVersion = 1;
	inline constexpr uint64 MaximumAuthoredBulkBytes = 1024ull * 1024ull * 1024ull;

	using FEditorBulkDataSource = FPackageResourceRange;

	namespace Private { struct FEditorBulkDataState; }

	// Owns authored content identity and an immutable memory or package-resource snapshot.
	class FEditorBulkData
	{
	public:
		ENGINE_API FEditorBulkData();
		ENGINE_API explicit FEditorBulkData(FGuid InstanceId);
		ENGINE_API FEditorBulkData(const FEditorBulkData& Other);
		ENGINE_API auto operator=(const FEditorBulkData& Other) -> FEditorBulkData&;
		ENGINE_API FEditorBulkData(FEditorBulkData&& Other) noexcept;
		ENGINE_API auto operator=(FEditorBulkData&& Other) noexcept -> FEditorBulkData&;

		ENGINE_API auto GetInstanceId() const -> FGuid;
		ENGINE_API auto GetPayloadId() const -> FXxHash128;
		ENGINE_API auto GetPayloadSize() const -> uint64;
		ENGINE_API auto IsMemoryResident() const -> bool;
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
		std::shared_ptr<const Private::FEditorBulkDataState> State;
	};
}
