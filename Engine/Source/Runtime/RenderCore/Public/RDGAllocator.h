#pragma once

#include "RDGDefinitions.h"
#include "RHICompletion.h"
#include <concepts>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Durin
{
	enum class ERDGAllocationError : uint8
	{
		AllocatorFailure,
		AllocationRetrySuppressed,
		AllocationKindInvalid,
		AllocationRetryDeferred,
		AllocationRetirementPending,
		PhysicalAllocationFailed,
		AllocationPublicationFailed,
	};
	RENDERCORE_API auto ToString(ERDGAllocationError Error) -> std::string_view;

	struct FRDGAllocationBudgetError
	{
		uint64 Actual = 0, Limit = 0;
	};
	RENDERCORE_API auto ToString(const FRDGAllocationBudgetError& Error) -> std::string;

	struct FRDGAllocationFailure
	{
		ERDGAllocationError Reason;
		uint32 ResourceId = UINT32_MAX;
		FRHICreationError Cause;
	};
	RENDERCORE_API auto ToString(const FRDGAllocationFailure& Error) -> std::string;

	struct FRDGAllocationError
	{
		using FDetail = std::variant<FRDGAllocationFailure, FRDGAllocationBudgetError>;
		FDetail Detail;
		template<typename T> requires std::constructible_from<FDetail, T>
		FRDGAllocationError(T Error) : Detail(std::move(Error)) {}
	};
	RENDERCORE_API auto ToString(const FRDGAllocationError& Error) -> std::string;
	using FRDGAllocationResult = std::expected<void, FRDGAllocationError>;

	// A successful terminal graph join proves completion of every using queue.
	// Unpublished, canceled and failed recordings never authorize pool reuse.
	class FRDGAllocationRetirement final
	{
	public:
		auto IsReusable() const -> bool { return Completion.GetState() == ERHIGPUSubmissionState::Complete; }
	private:
		friend class FRDGBuilder;
		FRHIGPUSyncPointRef Completion;
	};

	// Describes one retained graph-created resource for execution allocation.
	// Diagnostic names are deliberately absent from allocation identity.
	struct FRDGAllocationRequest final
	{
		uint32 ResourceId = 0;
		ERDGResourceKind Kind = ERDGResourceKind::Texture;
		FRHITextureDesc TextureDesc;
		FRHIBufferDesc BufferDesc;
		uint32 FirstPass = 0;
		uint32 LastPass = 0;
		uint32 ObservationTag = 0;
		// Exported allocations must leave the reusable pool before Allocate returns.
		// Their counted references own them thereafter; no implicit pool return.
		bool bExtracted = false;
		std::shared_ptr<const FRDGAllocationRetirement> Retirement;
	};

	struct FRDGAllocationStatistics final
	{
		uint32 ActiveResources = 0;
		uint32 RetainedResources = 0;
		uint64 ActiveBytes = 0;
		uint64 RetainedBytes = 0;
		uint64 PeakActiveBytes = 0;
		uint64 ReuseHits = 0;
		uint64 ReuseMisses = 0;
		uint64 Evictions = 0;
		uint64 Failures = 0;
	};

	// Owns one complete candidate allocation result until graph execution retires.
	class RENDERCORE_API FRDGAllocatedResources final
	{
	public:
		auto SetTexture(uint32 ResourceId, FTextureRHIRef Texture,
			uint64 AllocationId = 0,
			std::string_view Disposition = "allocated") -> bool;
		auto SetBuffer(uint32 ResourceId, FBufferRHIRef Buffer,
			uint64 AllocationId = 0,
			std::string_view Disposition = "allocated") -> bool;
		auto SetStatistics(const FRDGAllocationStatistics& InStatistics) -> void
		{
			Statistics = InStatistics;
		}

	private:
		friend class FRDGBuilder;
		explicit FRDGAllocatedResources(uint32 Count);
		std::vector<FTextureRHIRef> Textures;
		std::vector<FBufferRHIRef> Buffers;
		std::vector<uint64> AllocationIds;
		std::vector<std::string> AllocationDispositions;
		FRDGAllocationStatistics Statistics;
	};

	// Allocates one retained batch atomically. Implementations must not publish a
	// partial result when returning failure. bExtracted resources transfer out of
	// reusable ownership even if subsequent recording fails. Other allocations
	// are borrowed for one ordered Execute; retaining a reference is not an export.
	// Reuse requires ordered GPU uses on the same RHI timeline, or explicit GPU
	// completion/synchronization across timelines. CPU return is not GPU completion.
	class RENDERCORE_API FRDGAllocator
	{
	public:
		virtual ~FRDGAllocator() = default;
		// Opt in only when reuse accounts for all prior GPU queue uses.
		virtual auto SupportsAsyncCompute() const -> bool { return false; }
		virtual auto Allocate(std::span<const FRDGAllocationRequest> Requests,
			FRDGAllocatedResources& OutResources)
			-> FRDGAllocationResult = 0;
	};
} // namespace Durin
