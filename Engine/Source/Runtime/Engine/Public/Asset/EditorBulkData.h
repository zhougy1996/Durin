#pragma once

#include "EngineAPI.h"
#include "Asset/PackageResource.h"
#include "Serialization/Archive.h"
#include "Templates/AtomicSharedPtr.h"

namespace Durin
{
	inline constexpr uint32 EditorBulkDataContentIdVersion = 1;
	inline constexpr uint64 MaximumAuthoredBulkBytes = 1024ull * 1024ull * 1024ull;

	using FEditorBulkDataSource = FPackageResourceRange;

	enum class EEditorBulkDataError : uint8
	{
		None, PayloadSizeLimit, InvalidInstanceIdentity, MissingContentIdentity,
		LogicalSizeMismatch, InvalidRange,
	};
	struct FEditorBulkDataError
	{
		EEditorBulkDataError Code = EEditorBulkDataError::None;
		FGuid InstanceId;
		FXxHash128 ContentId;
		uint64 Actual = 0;
		uint64 Expected = 0;
		std::optional<FPackageResourceRangeError> RangeCause;
	};
	struct FEditorBulkDataResult
	{
		FEditorBulkDataError Error;
		auto Succeeded() const -> bool { return Error.Code == EEditorBulkDataError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	ENGINE_API auto FormatEditorBulkDataError(const FEditorBulkDataError& Error) -> std::string;

	namespace AssetPrivate { struct FEditorBulkDataState; }

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
		ENGINE_API auto UpdatePayload(FByteView Bytes) -> FEditorBulkDataResult;
		ENGINE_API auto UpdatePayload(FSharedByteBuffer Buffer) -> FEditorBulkDataResult;
		ENGINE_API static auto TryCreatePackageBacked(
			FGuid InstanceId,
			FXxHash128 ContentId,
			uint64 LogicalSize,
			FEditorBulkDataSource Source,
			FEditorBulkData& OutValue) -> FEditorBulkDataResult;

		ENGINE_API auto Serialize(FArchive& Ar) -> void;
		ENGINE_API auto Identical(const FEditorBulkData& Other) const -> bool;

	private:
		TAtomicSharedPtr<const AssetPrivate::FEditorBulkDataState> State;
	};
}
