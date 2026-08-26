#pragma once

#include "DynamicRHI.h"
#include "VulkanRHIAPI.h"

namespace Durin::VulkanRHI
{
	enum class EVulkanDebugMessageSeverity : uint8
	{
		Verbose,
		Information,
		Warning,
		Error,
	};

	struct FVulkanDebugMessageStatistics
	{
		uint64 TotalCount = 0;
		uint64 VerboseCount = 0;
		uint64 InformationCount = 0;
		uint64 WarningCount = 0;
		uint64 ErrorCount = 0;
		uint64 GeneralCount = 0;
		uint64 ValidationCount = 0;
		uint64 PerformanceCount = 0;
		uint64 TruncatedCount = 0;
		uint64 RecursionDropCount = 0;
	};

	struct FVulkanDiagnosticAvailability
	{
		bool bRequested = false;
		bool bDebugUtilsSupported = false;
		bool bDebugUtilsActive = false;
		bool bValidationLayerSupported = false;
		bool bValidationLayerActive = false;
		bool bMessengerActive = false;
	};

	struct FVulkanDebugUtilsStatistics
	{
		uint64 NamingAttemptCount = 0;
		uint64 NamingFailureCount = 0;
		uint64 NamingUnavailableSkipCount = 0;
		uint64 LabelBeginCount = 0;
		uint64 LabelEndCount = 0;
		uint64 LabelUnavailableSkipCount = 0;
		uint64 ActiveLabelDepth = 0;
		uint64 LabelHighWater = 0;
	};

	// Centralizes optional VK_EXT_debug_utils device dispatch and accounting.
	class VULKANRHI_API FVulkanDebugUtils
	{
	public:
		auto SetExtensionActive(bool bActive) -> void;
		auto InitializeDevice(vk::Device Device) -> void;
		auto ResetDevice() -> void;

		template<typename HandleType>
		auto NameObject(HandleType Handle, std::string_view Name) -> void
		{
			if (!Handle || Name.empty()) return;
			using NativeType = typename HandleType::NativeType;
			const NativeType NativeHandle = static_cast<NativeType>(Handle);
			uint64 Value = 0;
			if constexpr (std::is_pointer_v<NativeType>)
				Value = reinterpret_cast<uint64>(NativeHandle);
			else
				Value = static_cast<uint64>(NativeHandle);
			NameObjectRaw(HandleType::objectType, Value, Name);
		}

		auto BeginLabel(vk::CommandBuffer CommandBuffer,
			std::string_view Name) -> bool;
		auto EndLabel(vk::CommandBuffer CommandBuffer) -> void;
		auto MakeInternalName(std::string_view Role) -> std::string;
		auto Snapshot() const -> FVulkanDebugUtilsStatistics;
		auto ResetStatistics() -> void;

	private:
		static auto Increment(std::atomic<uint64>& Counter) -> void;
		auto NameObjectRaw(vk::ObjectType Type, uint64 Handle,
			std::string_view Name) -> void;

		vk::Device Device;
		PFN_vkSetDebugUtilsObjectNameEXT SetObjectName = nullptr;
		PFN_vkCmdBeginDebugUtilsLabelEXT BeginCommandLabel = nullptr;
		PFN_vkCmdEndDebugUtilsLabelEXT EndCommandLabel = nullptr;
		bool bExtensionActive = false;
		std::atomic<uint64> NextInternalNameIndex = 0;
		std::atomic<uint64> NamingAttemptCount = 0;
		std::atomic<uint64> NamingFailureCount = 0;
		std::atomic<uint64> NamingUnavailableSkipCount = 0;
		std::atomic<uint64> LabelBeginCount = 0;
		std::atomic<uint64> LabelEndCount = 0;
		std::atomic<uint64> LabelUnavailableSkipCount = 0;
		std::atomic<uint64> ActiveLabelDepth = 0;
		std::atomic<uint64> LabelHighWater = 0;
	};

	struct FVulkanClassifiedDebugMessage
	{
		static constexpr size_t MaximumMessageBytes = 4096;

		EVulkanDebugMessageSeverity Severity =
			EVulkanDebugMessageSeverity::Information;
		bool bGeneral = false;
		bool bValidation = false;
		bool bPerformance = false;
		bool bTruncated = false;
		std::string_view Message;
	};

	using FVulkanDebugMessageSink = void (*)(
		const FVulkanClassifiedDebugMessage&, void*);

	// Owns bounded atomics used by the arbitrary-thread Vulkan callback.
	class VULKANRHI_API FVulkanDebugCallbackState
	{
	public:
		auto HandleMessage(
			vk::DebugUtilsMessageSeverityFlagBitsEXT Severity,
			vk::DebugUtilsMessageTypeFlagsEXT Types,
			const char* Message,
			FVulkanDebugMessageSink Sink = nullptr,
			void* SinkUserData = nullptr) -> void;
		auto Snapshot() const -> FVulkanDebugMessageStatistics;
		auto Reset() -> void;

	public:
		static auto SaturatingIncrement(std::atomic<uint64>& Counter) -> void;
	private:
		std::atomic<uint64> TotalCount = 0;
		std::atomic<uint64> VerboseCount = 0;
		std::atomic<uint64> InformationCount = 0;
		std::atomic<uint64> WarningCount = 0;
		std::atomic<uint64> ErrorCount = 0;
		std::atomic<uint64> GeneralCount = 0;
		std::atomic<uint64> ValidationCount = 0;
		std::atomic<uint64> PerformanceCount = 0;
		std::atomic<uint64> TruncatedCount = 0;
		std::atomic<uint64> RecursionDropCount = 0;
	};

	VULKANRHI_API VKAPI_ATTR vk::Bool32 VKAPI_CALL VulkanDebugUtilsCallback(
		vk::DebugUtilsMessageSeverityFlagBitsEXT Severity,
		vk::DebugUtilsMessageTypeFlagsEXT Types,
		const vk::DebugUtilsMessengerCallbackDataEXT* CallbackData,
		void* UserData);

	// Selects the backend-private VMA placement and CPU access policy.
	enum class EVulkanAllocationClassCandidate : uint8
	{
		DeviceLocal,
		DynamicUpload,
		TransferUpload,
		TransferReadback,
		Count
	};

	struct FVulkanAllocationCandidateStatistics
	{
		static constexpr uint32 SizeBucketCount = 16;

		uint64 AllocationCount = 0;
		uint64 AllocationFailureCount = 0;
		uint64 DedicatedAllocationCount = 0;
		uint64 RequestedBytes = 0;
		uint64 ActualBytes = 0;
		uint64 LiveAllocationCount = 0;
		uint64 LiveBytes = 0;
		uint64 PeakLiveBytes = 0;
		uint64 PeakAllocationBytes = 0;
		uint64 ArenaCapacityBytes = 0;
		uint64 ArenaLiveBytes = 0;
		uint64 ArenaHighWaterBytes = 0;
		uint64 ArenaReuseCount = 0;
		uint64 ArenaOverflowCount = 0;
		uint64 ArenaOversizeCount = 0;
		uint64 ArenaWaitCount = 0;
		std::array<uint64, SizeBucketCount> SizeBuckets = {};
	};

	// Captures the pre-M4 allocation, transfer, wait, reuse, and retirement baseline.
	struct FVulkanMemoryBaselineStatistics
	{
		static constexpr uint32 AllocationClassCount =
			static_cast<uint32>(EVulkanAllocationClassCandidate::Count);
		static constexpr uint32 MaxMemoryHeaps = 16;

		std::array<FVulkanAllocationCandidateStatistics, AllocationClassCount>
			AllocationClasses = {};
		uint64 UploadOperationCount = 0;
		uint64 UploadBytes = 0;
		uint64 PeakUploadBytesPerFrame = 0;
		uint64 ReadbackOperationCount = 0;
		uint64 ReadbackBytes = 0;
		uint64 PeakReadbackBytesPerFrame = 0;
		uint64 FrameFenceWaitCount = 0;
		uint64 FrameFenceWaitNanoseconds = 0;
		uint64 CommandBufferAllocationCount = 0;
		uint64 CommandBufferReuseCount = 0;
		uint64 DescriptorPoolCount = 0;
		uint64 DescriptorPoolSetCapacity = 0;
		uint64 DescriptorAllocatedSetCount = 0;
		uint64 DescriptorPeakAllocatedSetCount = 0;
		uint64 DeferredDeletePendingCount = 0;
		uint64 DeferredDeleteHighWater = 0;
		uint64 DeferredDeleteReleasedCount = 0;
		uint64 DeferredDeleteMaxAgeFrames = 0;
		uint32 HeapCount = 0;
		std::array<uint64, MaxMemoryHeaps> HeapUsageBytes = {};
		std::array<uint64, MaxMemoryHeaps> HeapBudgetBytes = {};
	};

	// Serializes diagnostic updates from RHI and cross-thread deletion producers.
	class VULKANRHI_API FVulkanMemoryBaselineTracker
	{
	public:
		auto BeginFrame() -> void;
		auto RecordAllocation(EVulkanAllocationClassCandidate Candidate,
			uint64 RequestedBytes, uint64 ActualBytes, bool bDedicated) -> void;
		auto RecordAllocationFailure(EVulkanAllocationClassCandidate Candidate) -> void;
		auto RecordAllocationFreed(EVulkanAllocationClassCandidate Candidate,
			uint64 ActualBytes) -> void;
		auto RecordArenaPageAllocated(EVulkanAllocationClassCandidate Candidate,
			uint64 Bytes) -> void;
		auto RecordArenaPageFreed(EVulkanAllocationClassCandidate Candidate,
			uint64 Bytes) -> void;
		auto RecordArenaRangeAllocated(EVulkanAllocationClassCandidate Candidate,
			uint64 Bytes, bool bReuse, bool bOversize) -> void;
		auto RecordArenaRangeReclaimed(EVulkanAllocationClassCandidate Candidate,
			uint64 Bytes) -> void;
		auto RecordArenaOverflow(EVulkanAllocationClassCandidate Candidate) -> void;
		auto RecordArenaWait(EVulkanAllocationClassCandidate Candidate) -> void;
		auto RecordUpload(uint64 Bytes) -> void;
		auto RecordReadback(uint64 Bytes) -> void;
		auto RecordFrameFenceWait(uint64 Nanoseconds) -> void;
		auto RecordCommandBufferAllocation() -> void;
		auto RecordCommandBufferReuse() -> void;
		auto RecordDescriptorPoolCreated(uint64 SetCapacity) -> void;
		auto RecordDescriptorPoolDestroyed(
			uint64 SetCapacity, uint64 AllocatedSetCount) -> void;
		auto RecordDescriptorSetsAllocated(uint64 SetCount) -> void;
		auto RecordDescriptorPoolReset(uint64 ReleasedSetCount) -> void;
		auto RecordDeferredDeleteEnqueued() -> void;
		auto RecordDeferredDeletesReleased(uint64 Count, uint64 MaxAgeFrames) -> void;
		auto RecordHeapBudgets(std::span<const uint64> UsageBytes,
			std::span<const uint64> BudgetBytes) -> void;

		auto Snapshot() const -> FVulkanMemoryBaselineStatistics;
		auto ResetCounters() -> void;

		static auto SaturatingAdd(uint64 Left, uint64 Right) -> uint64;

	private:
		mutable std::mutex Mutex;
		FVulkanMemoryBaselineStatistics Statistics;
		uint64 CurrentFrameUploadBytes = 0;
		uint64 CurrentFrameReadbackBytes = 0;
	};

	extern FVulkanMemoryBaselineTracker GVulkanMemoryBaselineTracker;

	VULKANRHI_API auto GetVulkanMemoryBaselineStatistics()
		-> FVulkanMemoryBaselineStatistics;
	VULKANRHI_API auto ResetVulkanMemoryBaselineStatistics() -> void;
	VULKANRHI_API auto FormatVulkanMemoryBaselineStatistics(
		const FVulkanMemoryBaselineStatistics& Statistics) -> std::string;
	VULKANRHI_API auto GetRHIMemoryStatistics() -> FRHIMemoryStatistics;
} // namespace Durin::VulkanRHI
