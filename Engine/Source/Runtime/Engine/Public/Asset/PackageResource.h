#pragma once

#include "EngineAPI.h"
#include "Asset/PackageBulkData.h"
#include "Serialization/SharedByteBuffer.h"

namespace Durin::Asset
{
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

	namespace Private { struct FPackageResourceRequestState; }

	// Owns one exactly-once package range result and an advisory cancellation request.
	class FPackageResourceRequest
	{
	public:
		FPackageResourceRequest() = default;

		ENGINE_API auto IsReady() const -> bool;
		ENGINE_API auto Cancel() -> void;
		ENGINE_API auto Wait() const -> FPackageResourceReadResult;

		ENGINE_API static auto Completed(FPackageResourceReadResult Result)
			-> FPackageResourceRequest;
		ENGINE_API static auto Transform(
			FPackageResourceRequest Input,
			std::function<FPackageResourceReadResult(FPackageResourceReadResult)> Function)
			-> FPackageResourceRequest;

	private:
		explicit FPackageResourceRequest(
			std::shared_ptr<Private::FPackageResourceRequestState> InState)
			: State(std::move(InState)) {}

		std::shared_ptr<Private::FPackageResourceRequestState> State;

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
		std::vector<std::weak_ptr<Private::FPackageResourceRequestState>> Requests;
	};

	using FPackageResourceHandle = std::shared_ptr<FPackageResource>;

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

	// Owns loose package resources and retires them before package I/O shutdown.
	class FPackageResourceManager
	{
	public:
		ENGINE_API ~FPackageResourceManager();

		ENGINE_API auto RegisterLoosePackage(
			std::string LogicalPackageId,
			const std::filesystem::path& PackagePath,
			const FPackageBulkSegmentSummary& Summary,
			std::span<const FPackageBulkDataEntry> Entries,
			FPackageResourceHandle& OutHandle,
			std::string* OutError = nullptr) -> bool;
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
