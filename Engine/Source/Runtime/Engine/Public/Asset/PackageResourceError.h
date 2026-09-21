#pragma once

#include <expected>

#include "EngineAPI.h"
#include "Asset/EditorBulkDataStorageError.h"
#include "Asset/PackageBulkData.h"
#include "Misc/FileIO.h"
#include "Serialization/SharedByteBuffer.h"

namespace Durin
{
	enum class ETaskState : uint8;
	enum class ETaskWaitStatus : uint8;
	enum class EBulkDataState : uint8;
	// Classification of a completed range read; readiness belongs to its request.
	enum class EPackageResourceReadStatus : uint8
	{
		Success, InvalidRange, MissingSegment, TruncatedSegment,
		SegmentDigestMismatch, Cancelled, Retired, IoError,
	};
	enum class EPackageResourceReadReason : uint8
	{
		None, Cancelled, Retired, InvalidRequest, InvalidRange, PackageBusy,
		QuerySize, ChangedBeforeRead, Open, Seek, ShortRead, ChangedDuringRead,
		TaskCancelled, TaskFailed, WaitRejected, MissingSource, ContentMismatch,
		LogicalSizeMismatch, BulkUnavailable, ReloadUnavailable,
	};
	struct FPackageResourceReadError
	{
		EPackageResourceReadStatus Status = EPackageResourceReadStatus::IoError;
		EPackageResourceReadReason Reason = EPackageResourceReadReason::None;
		std::filesystem::path Path;
		uint64 Offset = 0;
		uint64 Size = 0;
		uint64 Extent = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		FXxHash128 ActualDigest;
		FXxHash128 ExpectedDigest;
		std::error_code SystemError;
		uint32 StreamState = 0;
		std::optional<ETaskState> TaskState;
		std::optional<ETaskWaitStatus> WaitStatus;
		std::optional<EBulkDataState> BulkState;
	};
	using FPackageResourceReadResult = std::expected<FSharedByteBuffer, FPackageResourceReadError>;
	inline auto GetPackageResourceReadStatus(const FPackageResourceReadResult& Result) -> EPackageResourceReadStatus
	{
		return Result ? EPackageResourceReadStatus::Success : Result.error().Status;
	}

	struct FAssetReadError;
	enum class EPreparedPackageResourceError : uint8 { None, InvalidClosure, BudgetExceeded, IoError, Stale, Cancelled };
	enum class EPreparedPackageResourceReason : uint8
	{
		None, PackageBusy, Cancelled, InvalidLogicalPath, InvalidMain, EmptyPrepared,
		MainBudget, ClosureBudget, Allocation, FileIo, FileSystem, ExtentChanged,
		DigestChanged, ResolveCodec, ReadHeader, Inspect, BulkStorage, BulkValidation,
		BulkExtent, UndeclaredBulk,
	};
	struct FPreparedPackageResourceError
	{
		EPreparedPackageResourceError Code = EPreparedPackageResourceError::None;
		EPreparedPackageResourceReason Reason = EPreparedPackageResourceReason::None;
		std::filesystem::path Path;
		uint64 MainBytes = 0;
		uint64 BulkBytes = 0;
		uint64 MaximumBytes = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		FXxHash128 ActualDigest;
		FXxHash128 ExpectedDigest;
		// A preparation failure has at most one lower-layer cause.
		std::variant<std::monostate, FFileIO::FFileError, FPackageBulkDataError,
			FEditorBulkDataStorageError, std::shared_ptr<const FAssetReadError>> Cause;
	};
	ENGINE_API auto FormatPreparedPackageResourceError(const FPreparedPackageResourceError& Error) -> std::string;

	// Bulk validation adds the source path; file errors already own it.
	struct FPackageBulkValidationFailure
	{
		std::filesystem::path Path;
		FPackageBulkDataError Error;
	};
	using FPackageGenerationError = std::variant<FFileIO::FFileError, FPackageBulkValidationFailure>;
	using FPackageGenerationResult = std::expected<void, FPackageGenerationError>;
	enum class EPackageResourceRegistrationError : uint8
	{
		None, PackageBusy, EmptySegment, InvalidMetadata, ShuttingDown,
		InvalidGeneration,
	};
	struct FPackageResourceRegistrationError
	{
		EPackageResourceRegistrationError Code = EPackageResourceRegistrationError::None;
		std::filesystem::path Path;
		std::optional<FPackageBulkDataError> BulkCause;
		std::optional<FPackageGenerationError> PrimaryCause;
	};
	ENGINE_API auto FormatPackageResourceRegistrationError(const FPackageResourceRegistrationError& Error) -> std::string;
}
