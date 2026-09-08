#pragma once

#include "EngineAPI.h"
#include "Asset/PackageBulkData.h"
#include "Serialization/SharedByteBuffer.h"
#include "Misc/FileHelper.h"

namespace Durin
{
	class FPackagePath;

	enum class EPackageResourceReadStatus : uint8
	{
		Pending,
		Success,
		InvalidRange,
		MissingSegment,
		TruncatedSegment,
		SegmentDigestMismatch,
		Cancelled,
		Retired,
		IoError,
	};

	struct FPackageResourceReadResult
	{
		EPackageResourceReadStatus Status = EPackageResourceReadStatus::Pending;
		FSharedByteBuffer Buffer;
		std::string Message;

		auto Succeeded() const -> bool { return Status == EPackageResourceReadStatus::Success; }
		explicit operator bool() const { return Succeeded(); }
	};

	struct FPackageResourceReadStats
	{
		uint64 ValidationReadCount = 0;
		uint64 ValidationBytesRead = 0;
		uint64 PeakValidationScratchBytes = 0;
		uint64 RequestCount = 0;
		uint64 RequestedBytes = 0;
	};

	namespace AssetPrivate { struct FPackageResourceRequestState; }

	// Owns one exactly-once package range result and an advisory cancellation request.
	class FPackageResourceRequest
	{
	public:
		FPackageResourceRequest() = default;

		ENGINE_API auto IsReady() const -> bool;
		ENGINE_API auto Cancel() -> void;
		// A rejected thread/dependency wait returns a caller-local IoError; it
		// does not terminalize the request or prevent a later authorized wait.
		ENGINE_API auto Wait() const -> FPackageResourceReadResult;

		ENGINE_API static auto Completed(FPackageResourceReadResult Result)
			-> FPackageResourceRequest;
		ENGINE_API static auto Transform(
			FPackageResourceRequest Input,
			std::function<FPackageResourceReadResult(FPackageResourceReadResult)> Function)
			-> FPackageResourceRequest;

	private:
		explicit FPackageResourceRequest(
			std::shared_ptr<AssetPrivate::FPackageResourceRequestState> InState)
			: State(std::move(InState)) {}

		std::shared_ptr<AssetPrivate::FPackageResourceRequestState> State;

		friend class FPackageResource;
	};

	// Serves bounded reads from one already validated logical package bulk segment.
	class FPackageResource : public std::enable_shared_from_this<FPackageResource>
	{
	public:
		ENGINE_API virtual ~FPackageResource();

		ENGINE_API auto ReadRangeAsync(uint64 Offset, uint64 Size)
			-> FPackageResourceRequest;
		ENGINE_API auto ReadRange(uint64 Offset, uint64 Size)
			-> FPackageResourceReadResult;
		ENGINE_API auto Retire() -> void;
		ENGINE_API auto IsRetired() const -> bool;
		ENGINE_API auto GetReadStats() const -> FPackageResourceReadStats;
		auto GetSegmentExtent() const -> uint64 { return SegmentExtent; }

	protected:
		explicit FPackageResource(uint64 InSegmentExtent,
			FPackageResourceReadStats InReadStats = {})
			: SegmentExtent(InSegmentExtent), ReadStats(InReadStats) {}

		virtual auto ReadRangeImpl(
			uint64 Offset,
			uint64 Size,
			const std::atomic_bool& bCancelled) -> FPackageResourceReadResult = 0;

	private:
		uint64 SegmentExtent = 0;
		mutable std::mutex Mutex;
		bool bRetired = false;
		FPackageResourceReadStats ReadStats;
		std::vector<std::weak_ptr<AssetPrivate::FPackageResourceRequestState>> Requests;
	};

	using FPackageResourceHandle = std::shared_ptr<FPackageResource>;

	// Storage preparation outcome; Ready does not imply graph/runtime readiness.
	enum class EPreparedPackageResourceStatus : uint8
	{
		Ready,
		InvalidClosure,
		BudgetExceeded,
		IoError,
		Stale,
		Cancelled,
	};

	// Reports a preparation/revalidation failure without changing the output owner.
	struct FPreparedPackageResourceResult
	{
		EPreparedPackageResourceStatus Status = EPreparedPackageResourceStatus::Ready;
		std::string Message;
		explicit operator bool() const { return Status == EPreparedPackageResourceStatus::Ready; }
	};

	// Owns an unpublished immutable main/bulk closure. Main schema validation
	// belongs to the existing codec; this owner preserves the validated bytes.
	class FPreparedPackageResource
	{
	public:
		// Reads and validates through the existing package codec without creating
		// objects. The budget bounds retained bytes, not parser scratch allocations.
		ENGINE_API static auto Read(
			const FPackagePath& LogicalPath,
			const std::filesystem::path& PackagePath,
			uint64 MaximumRetainedBytes,
			FPreparedPackageResource& Out,
			const std::function<bool()>& IsCancelled = {}) -> FPreparedPackageResourceResult;

		// Requires main bytes and directory facts already validated by the codec.
		// Failure leaves Out unchanged. No registry publication or disk recovery.
		// The byte budget covers retained main/bulk bytes; callers separately
		// account decoded values, object graphs, and runtime products.
		ENGINE_API static auto Prepare(
			const std::filesystem::path& PackagePath,
			FSharedByteBuffer ValidatedMain,
			const FPackageBulkSegmentSummary& Summary,
			std::span<const FPackageBulkDataEntry> Entries,
			uint64 MaximumRetainedBytes,
			FPreparedPackageResource& Out,
			const std::function<bool()>& IsCancelled = {}) -> FPreparedPackageResourceResult;

		// Rehashes both physical files with bounded scratch. Owners must still
		// hold their save/edit lease between this check and memory publication.
		ENGINE_API auto Revalidate(const std::function<bool()>& IsCancelled = {}) const
			-> FPreparedPackageResourceResult;
		auto GetMainBytes() const -> const FSharedByteBuffer& { return MainBytes; }
		auto GetBulkResource() const -> const FPackageResourceHandle& { return BulkResource; }
		auto GetRetainedBytes() const -> uint64 { return MainBytes.GetSize() + BulkExtent; }

	private:
		std::filesystem::path MainPath;
		FSharedByteBuffer MainBytes;
		FPackageResourceHandle BulkResource;
		FXxHash128 MainDigest;
		FXxHash128 BulkDigest;
		uint64 BulkExtent = 0;
	};

	// Copies and validates one bulk generation before exposing it to lazy readers.
	// The caller keeps Segment stable during this call; successful reads retain
	// owned bytes without filesystem access or global resource registration.
	ENGINE_API auto CreateOwnedPackageResource(
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries,
		FByteView Segment,
		FPackageResourceHandle& OutHandle,
		std::string* OutError = nullptr) -> bool;

	// Identifies one bounded stored range in a validated logical package segment.
	struct FPackageResourceRange
	{
		FPackageResourceHandle Resource;
		uint64 SegmentOffset = 0;
		uint64 StoredSize = 0;
		uint32 StorageFlags = 0;
		uint32 Alignment = 1;
	};

	// Validates storage facts only; the caller retains logical-size and domain limits.
	ENGINE_API auto ValidatePackageResourceRange(
		const FPackageResourceRange& Range,
		uint64 MaximumStoredSize,
		std::string* OutError = nullptr) -> bool;

	enum class EPackageResourceRegistrationStatus : uint8
	{
		Success,
		InvalidMetadata,
		InvalidGeneration,
		RecoveryFailed,
		ShuttingDown,
	};

	// Owns a published resource or the stage and causes of failed registration.
	struct [[nodiscard]] FPackageResourceRegistrationResult
	{
		EPackageResourceRegistrationStatus Status = EPackageResourceRegistrationStatus::InvalidMetadata;
		FPackageResourceHandle Resource;
		std::string Message;
		FFileHelper::FAtomicFileError PublicationError;
		explicit operator bool() const { return Status == EPackageResourceRegistrationStatus::Success; }
	};

	// Owns loose package resources and retires them before package I/O shutdown.
	class FPackageResourceManager
	{
	public:
		ENGINE_API ~FPackageResourceManager();

		ENGINE_API auto RegisterLoosePackage(
			std::string LogicalPackageId,
			const std::filesystem::path& PackagePath,
			const FPackageBulkSegmentSummary& Summary,
			std::span<const FPackageBulkDataEntry> Entries
		) -> FPackageResourceRegistrationResult;
		ENGINE_API auto RetirePackage(std::string_view LogicalPackageId) -> void;
		ENGINE_API auto FindPackage(std::string_view LogicalPackageId) const
			-> FPackageResourceHandle;
		ENGINE_API auto GetRegisteredPackageCount() const -> uint64;
		ENGINE_API auto RetireAllPackages() -> void;
		ENGINE_API auto Shutdown() -> void;

	private:
		mutable std::mutex Mutex;
		bool bShutdown = false;
		std::unordered_map<std::string, FPackageResourceHandle> Resources;
	};

	ENGINE_API auto GetPackageResourceManager() -> FPackageResourceManager&;
}
