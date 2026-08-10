#include "VulkanDiagnostics.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanMemoryBaselineTracker GVulkanMemoryBaselineTracker;

	namespace
	{
		thread_local bool GInsideVulkanDebugCallback = false;

		auto BoundUtf8(std::string_view Text, size_t MaximumBytes) -> std::string
		{
			if (Text.size() <= MaximumBytes) return std::string(Text);
			size_t End = MaximumBytes;
			while (End > 0
				&& (static_cast<unsigned char>(Text[End]) & 0xc0u) == 0x80u)
			{
				--End;
			}
			return std::string(Text.substr(0, End));
		}

		auto LogVulkanDebugMessage(
			const FVulkanClassifiedDebugMessage& Message, void*) -> void
		{
			switch (Message.Severity)
			{
			case EVulkanDebugMessageSeverity::Verbose:
				DURIN_TRACE("Vulkan diagnostic: {}", Message.Message);
				break;
			case EVulkanDebugMessageSeverity::Information:
				DURIN_DEBUG("Vulkan diagnostic: {}", Message.Message);
				break;
			case EVulkanDebugMessageSeverity::Warning:
				DURIN_WARN("Vulkan diagnostic: {}", Message.Message);
				break;
			case EVulkanDebugMessageSeverity::Error:
				DURIN_ERROR("Vulkan diagnostic: {}", Message.Message);
				break;
			}
		}

		auto GetAllocationSizeBucket(uint64 Size) -> uint32
		{
			uint32 Bucket = 0;
			uint64 UpperBound = 4096;
			while (Size > UpperBound
				&& Bucket + 1 < FVulkanAllocationCandidateStatistics::SizeBucketCount)
			{
				++Bucket;
				UpperBound = FVulkanMemoryBaselineTracker::SaturatingAdd(
					UpperBound, UpperBound);
			}
			return Bucket;
		}
	}

	auto FVulkanDebugCallbackState::SaturatingIncrement(
		std::atomic<uint64>& Counter) -> void
	{
		uint64 Current = Counter.load(std::memory_order_relaxed);
		while (Current != std::numeric_limits<uint64>::max()
			&& !Counter.compare_exchange_weak(Current, Current + 1,
				std::memory_order_relaxed, std::memory_order_relaxed))
		{
		}
	}

	auto FVulkanDebugUtils::Increment(std::atomic<uint64>& Counter) -> void
	{
		FVulkanDebugCallbackState::SaturatingIncrement(Counter);
	}

	auto FVulkanDebugUtils::SetExtensionActive(bool bActive) -> void
	{
		bExtensionActive = bActive;
	}

	auto FVulkanDebugUtils::InitializeDevice(vk::Device InDevice) -> void
	{
		Device = InDevice;
		if (!Device || !bExtensionActive) return;
		const VkDevice NativeDevice = static_cast<VkDevice>(Device);
		SetObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
			vkGetDeviceProcAddr(NativeDevice, "vkSetDebugUtilsObjectNameEXT"));
		BeginCommandLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
			vkGetDeviceProcAddr(NativeDevice, "vkCmdBeginDebugUtilsLabelEXT"));
		EndCommandLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
			vkGetDeviceProcAddr(NativeDevice, "vkCmdEndDebugUtilsLabelEXT"));
	}

	auto FVulkanDebugUtils::ResetDevice() -> void
	{
		Device = nullptr;
		SetObjectName = nullptr;
		BeginCommandLabel = nullptr;
		EndCommandLabel = nullptr;
	}

	auto FVulkanDebugUtils::NameObjectRaw(
		vk::ObjectType Type, uint64 Handle, std::string_view Name) -> void
	{
		Increment(NamingAttemptCount);
		if (!Device || !SetObjectName)
		{
			Increment(NamingUnavailableSkipCount);
			return;
		}
		const std::string BoundedName = BoundUtf8(Name, 255);
		VkDebugUtilsObjectNameInfoEXT Info{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, nullptr,
			static_cast<VkObjectType>(Type), Handle, BoundedName.c_str()};
		const VkResult Result = SetObjectName(
			static_cast<VkDevice>(Device), &Info);
		if (Result != VK_SUCCESS) Increment(NamingFailureCount);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		else RecordVulkanDebugUtilsEventForTest(
			EVulkanDebugUtilsTestEventType::ObjectName, Type, BoundedName);
#endif
	}

	auto FVulkanDebugUtils::BeginLabel(
		vk::CommandBuffer CommandBuffer, std::string_view Name) -> bool
	{
		if (!CommandBuffer || Name.empty()) return false;
		if (!BeginCommandLabel)
		{
			Increment(LabelUnavailableSkipCount);
			return false;
		}
		const std::string BoundedName = BoundUtf8(Name, 255);
		VkDebugUtilsLabelEXT Label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
			nullptr, BoundedName.c_str(), {0.18f, 0.55f, 0.95f, 1.0f}};
		BeginCommandLabel(static_cast<VkCommandBuffer>(CommandBuffer), &Label);
		Increment(LabelBeginCount);
		const uint64 Depth = ActiveLabelDepth.fetch_add(
			1, std::memory_order_relaxed) + 1;
		uint64 HighWater = LabelHighWater.load(std::memory_order_relaxed);
		while (HighWater < Depth && !LabelHighWater.compare_exchange_weak(
			HighWater, Depth, std::memory_order_relaxed,
			std::memory_order_relaxed)) {}
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		RecordVulkanDebugUtilsEventForTest(
			EVulkanDebugUtilsTestEventType::LabelBegin,
			vk::ObjectType::eCommandBuffer, BoundedName);
#endif
		return true;
	}

	auto FVulkanDebugUtils::EndLabel(vk::CommandBuffer CommandBuffer) -> void
	{
		if (!CommandBuffer || !EndCommandLabel)
		{
			Increment(LabelUnavailableSkipCount);
			return;
		}
		EndCommandLabel(static_cast<VkCommandBuffer>(CommandBuffer));
		Increment(LabelEndCount);
		const uint64 PreviousDepth = ActiveLabelDepth.fetch_sub(
			1, std::memory_order_relaxed);
		check(PreviousDepth != 0);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		RecordVulkanDebugUtilsEventForTest(
			EVulkanDebugUtilsTestEventType::LabelEnd,
			vk::ObjectType::eCommandBuffer, {});
#endif
	}

	auto FVulkanDebugUtils::MakeInternalName(std::string_view Role) -> std::string
	{
		const uint64 Index = NextInternalNameIndex.fetch_add(
			1, std::memory_order_relaxed) + 1;
		return std::format("Durin.{}.{}", Role, Index);
	}

	auto FVulkanDebugUtils::Snapshot() const -> FVulkanDebugUtilsStatistics
	{
		return {
			.NamingAttemptCount = NamingAttemptCount.load(std::memory_order_relaxed),
			.NamingFailureCount = NamingFailureCount.load(std::memory_order_relaxed),
			.NamingUnavailableSkipCount = NamingUnavailableSkipCount.load(std::memory_order_relaxed),
			.LabelBeginCount = LabelBeginCount.load(std::memory_order_relaxed),
			.LabelEndCount = LabelEndCount.load(std::memory_order_relaxed),
			.LabelUnavailableSkipCount = LabelUnavailableSkipCount.load(std::memory_order_relaxed),
			.ActiveLabelDepth = ActiveLabelDepth.load(std::memory_order_relaxed),
			.LabelHighWater = LabelHighWater.load(std::memory_order_relaxed),
		};
	}

	auto FVulkanDebugUtils::ResetStatistics() -> void
	{
		NamingAttemptCount.store(0, std::memory_order_relaxed);
		NamingFailureCount.store(0, std::memory_order_relaxed);
		NamingUnavailableSkipCount.store(0, std::memory_order_relaxed);
		LabelBeginCount.store(0, std::memory_order_relaxed);
		LabelEndCount.store(0, std::memory_order_relaxed);
		LabelUnavailableSkipCount.store(0, std::memory_order_relaxed);
		LabelHighWater.store(
			ActiveLabelDepth.load(std::memory_order_relaxed),
			std::memory_order_relaxed);
	}

	auto FVulkanDebugCallbackState::HandleMessage(
		vk::DebugUtilsMessageSeverityFlagBitsEXT Severity,
		vk::DebugUtilsMessageTypeFlagsEXT Types,
		const char* Message,
		FVulkanDebugMessageSink Sink,
		void* SinkUserData) -> void
	{
		if (GInsideVulkanDebugCallback)
		{
			SaturatingIncrement(RecursionDropCount);
			return;
		}
		struct FRecursionScope
		{
			FRecursionScope() { GInsideVulkanDebugCallback = true; }
			~FRecursionScope() { GInsideVulkanDebugCallback = false; }
		} RecursionScope;

		FVulkanClassifiedDebugMessage Classified;
		switch (Severity)
		{
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
			Classified.Severity = EVulkanDebugMessageSeverity::Verbose;
			SaturatingIncrement(VerboseCount);
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
			Classified.Severity = EVulkanDebugMessageSeverity::Warning;
			SaturatingIncrement(WarningCount);
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
			Classified.Severity = EVulkanDebugMessageSeverity::Error;
			SaturatingIncrement(ErrorCount);
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
		default:
			Classified.Severity = EVulkanDebugMessageSeverity::Information;
			SaturatingIncrement(InformationCount);
			break;
		}
		SaturatingIncrement(TotalCount);

		Classified.bGeneral = static_cast<bool>(
			Types & vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral);
		Classified.bValidation = static_cast<bool>(
			Types & vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
		Classified.bPerformance = static_cast<bool>(
			Types & vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance);
		if (!Classified.bGeneral && !Classified.bValidation
			&& !Classified.bPerformance)
		{
			Classified.bGeneral = true;
		}
		if (Classified.bGeneral) SaturatingIncrement(GeneralCount);
		if (Classified.bValidation) SaturatingIncrement(ValidationCount);
		if (Classified.bPerformance) SaturatingIncrement(PerformanceCount);

		const char* SafeMessage = Message ? Message : "<no message>";
		size_t MessageLength = 0;
		while (MessageLength <= FVulkanClassifiedDebugMessage::MaximumMessageBytes
			&& SafeMessage[MessageLength] != '\0')
		{
			++MessageLength;
		}
		if (MessageLength > FVulkanClassifiedDebugMessage::MaximumMessageBytes)
		{
			MessageLength = FVulkanClassifiedDebugMessage::MaximumMessageBytes;
			Classified.bTruncated = true;
			SaturatingIncrement(TruncatedCount);
		}
		Classified.Message = std::string_view(SafeMessage, MessageLength);
		if (Sink) Sink(Classified, SinkUserData);
	}

	auto FVulkanDebugCallbackState::Snapshot() const
		-> FVulkanDebugMessageStatistics
	{
		return {
			.TotalCount = TotalCount.load(std::memory_order_relaxed),
			.VerboseCount = VerboseCount.load(std::memory_order_relaxed),
			.InformationCount = InformationCount.load(std::memory_order_relaxed),
			.WarningCount = WarningCount.load(std::memory_order_relaxed),
			.ErrorCount = ErrorCount.load(std::memory_order_relaxed),
			.GeneralCount = GeneralCount.load(std::memory_order_relaxed),
			.ValidationCount = ValidationCount.load(std::memory_order_relaxed),
			.PerformanceCount = PerformanceCount.load(std::memory_order_relaxed),
			.TruncatedCount = TruncatedCount.load(std::memory_order_relaxed),
			.RecursionDropCount = RecursionDropCount.load(std::memory_order_relaxed),
		};
	}

	auto FVulkanDebugCallbackState::Reset() -> void
	{
		TotalCount.store(0, std::memory_order_relaxed);
		VerboseCount.store(0, std::memory_order_relaxed);
		InformationCount.store(0, std::memory_order_relaxed);
		WarningCount.store(0, std::memory_order_relaxed);
		ErrorCount.store(0, std::memory_order_relaxed);
		GeneralCount.store(0, std::memory_order_relaxed);
		ValidationCount.store(0, std::memory_order_relaxed);
		PerformanceCount.store(0, std::memory_order_relaxed);
		TruncatedCount.store(0, std::memory_order_relaxed);
		RecursionDropCount.store(0, std::memory_order_relaxed);
	}

	VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugUtilsCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT Severity,
		VkDebugUtilsMessageTypeFlagsEXT Types,
		const VkDebugUtilsMessengerCallbackDataEXT* CallbackData,
		void* UserData)
	{
		if (auto* State = static_cast<FVulkanDebugCallbackState*>(UserData))
		{
			State->HandleMessage(
				static_cast<vk::DebugUtilsMessageSeverityFlagBitsEXT>(Severity),
				static_cast<vk::DebugUtilsMessageTypeFlagsEXT>(Types),
				CallbackData ? CallbackData->pMessage : nullptr,
				&LogVulkanDebugMessage);
		}
		return VK_FALSE;
	}

	auto FVulkanMemoryBaselineTracker::SaturatingAdd(
		uint64 Left, uint64 Right) -> uint64
	{
		const uint64 Maximum = std::numeric_limits<uint64>::max();
		return Right > Maximum - Left ? Maximum : Left + Right;
	}

	auto FVulkanMemoryBaselineTracker::BeginFrame() -> void
	{
		std::lock_guard Lock(Mutex);
		CurrentFrameUploadBytes = 0;
		CurrentFrameReadbackBytes = 0;
	}

	auto FVulkanMemoryBaselineTracker::RecordAllocation(
		EVulkanAllocationClassCandidate Candidate,
		uint64 RequestedBytes,
		uint64 ActualBytes,
		bool bDedicated) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Class = Statistics.AllocationClasses[static_cast<uint32>(Candidate)];
		Class.AllocationCount = SaturatingAdd(Class.AllocationCount, 1);
		Class.RequestedBytes = SaturatingAdd(Class.RequestedBytes, RequestedBytes);
		Class.ActualBytes = SaturatingAdd(Class.ActualBytes, ActualBytes);
		Class.LiveAllocationCount = SaturatingAdd(Class.LiveAllocationCount, 1);
		Class.LiveBytes = SaturatingAdd(Class.LiveBytes, ActualBytes);
		Class.PeakLiveBytes = std::max(Class.PeakLiveBytes, Class.LiveBytes);
		if (bDedicated)
		{
			Class.DedicatedAllocationCount = SaturatingAdd(
				Class.DedicatedAllocationCount, 1);
		}
		Class.PeakAllocationBytes = std::max(Class.PeakAllocationBytes, ActualBytes);
		auto& Bucket = Class.SizeBuckets[GetAllocationSizeBucket(RequestedBytes)];
		Bucket = SaturatingAdd(Bucket, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordAllocationFailure(
		EVulkanAllocationClassCandidate Candidate) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Count = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.AllocationFailureCount;
		Count = SaturatingAdd(Count, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordAllocationFreed(
		EVulkanAllocationClassCandidate Candidate, uint64 ActualBytes) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Class = Statistics.AllocationClasses[static_cast<uint32>(Candidate)];
		Class.LiveAllocationCount = Class.LiveAllocationCount == 0
			? 0 : Class.LiveAllocationCount - 1;
		Class.LiveBytes = ActualBytes >= Class.LiveBytes
			? 0 : Class.LiveBytes - ActualBytes;
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaPageAllocated(
		EVulkanAllocationClassCandidate Candidate, uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaCapacityBytes;
		Value = SaturatingAdd(Value, Bytes);
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaPageFreed(
		EVulkanAllocationClassCandidate Candidate, uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaCapacityBytes;
		Value = Bytes >= Value ? 0 : Value - Bytes;
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaRangeAllocated(
		EVulkanAllocationClassCandidate Candidate, uint64 Bytes,
		bool bReuse, bool bOversize) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Class = Statistics.AllocationClasses[static_cast<uint32>(Candidate)];
		Class.ArenaLiveBytes = SaturatingAdd(Class.ArenaLiveBytes, Bytes);
		Class.ArenaHighWaterBytes = std::max(
			Class.ArenaHighWaterBytes, Class.ArenaLiveBytes);
		if (bReuse)
		{
			Class.ArenaReuseCount = SaturatingAdd(Class.ArenaReuseCount, 1);
		}
		if (bOversize)
		{
			Class.ArenaOversizeCount = SaturatingAdd(Class.ArenaOversizeCount, 1);
		}
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaRangeReclaimed(
		EVulkanAllocationClassCandidate Candidate, uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaLiveBytes;
		Value = Bytes >= Value ? 0 : Value - Bytes;
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaOverflow(
		EVulkanAllocationClassCandidate Candidate) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaOverflowCount;
		Value = SaturatingAdd(Value, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaWait(
		EVulkanAllocationClassCandidate Candidate) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaWaitCount;
		Value = SaturatingAdd(Value, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordUpload(uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.UploadOperationCount = SaturatingAdd(
			Statistics.UploadOperationCount, 1);
		Statistics.UploadBytes = SaturatingAdd(Statistics.UploadBytes, Bytes);
		CurrentFrameUploadBytes = SaturatingAdd(CurrentFrameUploadBytes, Bytes);
		Statistics.PeakUploadBytesPerFrame = std::max(
			Statistics.PeakUploadBytesPerFrame, CurrentFrameUploadBytes);
	}

	auto FVulkanMemoryBaselineTracker::RecordReadback(uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.ReadbackOperationCount = SaturatingAdd(
			Statistics.ReadbackOperationCount, 1);
		Statistics.ReadbackBytes = SaturatingAdd(Statistics.ReadbackBytes, Bytes);
		CurrentFrameReadbackBytes = SaturatingAdd(CurrentFrameReadbackBytes, Bytes);
		Statistics.PeakReadbackBytesPerFrame = std::max(
			Statistics.PeakReadbackBytesPerFrame, CurrentFrameReadbackBytes);
	}

	auto FVulkanMemoryBaselineTracker::RecordFrameFenceWait(uint64 Nanoseconds) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.FrameFenceWaitCount = SaturatingAdd(
			Statistics.FrameFenceWaitCount, 1);
		Statistics.FrameFenceWaitNanoseconds = SaturatingAdd(
			Statistics.FrameFenceWaitNanoseconds, Nanoseconds);
	}

	auto FVulkanMemoryBaselineTracker::RecordCommandBufferAllocation() -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.CommandBufferAllocationCount = SaturatingAdd(
			Statistics.CommandBufferAllocationCount, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordCommandBufferReuse() -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.CommandBufferReuseCount = SaturatingAdd(
			Statistics.CommandBufferReuseCount, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordDescriptorPoolCreated(
		uint64 SetCapacity) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DescriptorPoolCount = SaturatingAdd(
			Statistics.DescriptorPoolCount, 1);
		Statistics.DescriptorPoolSetCapacity = SaturatingAdd(
			Statistics.DescriptorPoolSetCapacity, SetCapacity);
	}

	auto FVulkanMemoryBaselineTracker::RecordDescriptorSetsAllocated(
		uint64 SetCount) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DescriptorAllocatedSetCount = SaturatingAdd(
			Statistics.DescriptorAllocatedSetCount, SetCount);
		Statistics.DescriptorPeakAllocatedSetCount = std::max(
			Statistics.DescriptorPeakAllocatedSetCount,
			Statistics.DescriptorAllocatedSetCount);
	}

	auto FVulkanMemoryBaselineTracker::RecordDescriptorPoolDestroyed(
		uint64 SetCapacity, uint64 AllocatedSetCount) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DescriptorPoolCount =
			Statistics.DescriptorPoolCount == 0
			? 0 : Statistics.DescriptorPoolCount - 1;
		Statistics.DescriptorPoolSetCapacity =
			SetCapacity >= Statistics.DescriptorPoolSetCapacity
			? 0 : Statistics.DescriptorPoolSetCapacity - SetCapacity;
		Statistics.DescriptorAllocatedSetCount =
			AllocatedSetCount >= Statistics.DescriptorAllocatedSetCount
			? 0 : Statistics.DescriptorAllocatedSetCount - AllocatedSetCount;
	}

	auto FVulkanMemoryBaselineTracker::RecordDescriptorPoolReset(
		uint64 ReleasedSetCount) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DescriptorAllocatedSetCount =
			ReleasedSetCount >= Statistics.DescriptorAllocatedSetCount
			? 0 : Statistics.DescriptorAllocatedSetCount - ReleasedSetCount;
	}

	auto FVulkanMemoryBaselineTracker::RecordDeferredDeleteEnqueued() -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DeferredDeletePendingCount = SaturatingAdd(
			Statistics.DeferredDeletePendingCount, 1);
		Statistics.DeferredDeleteHighWater = std::max(
			Statistics.DeferredDeleteHighWater,
			Statistics.DeferredDeletePendingCount);
	}

	auto FVulkanMemoryBaselineTracker::RecordDeferredDeletesReleased(
		uint64 Count, uint64 MaxAgeFrames) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DeferredDeletePendingCount =
			Count >= Statistics.DeferredDeletePendingCount
			? 0 : Statistics.DeferredDeletePendingCount - Count;
		Statistics.DeferredDeleteReleasedCount = SaturatingAdd(
			Statistics.DeferredDeleteReleasedCount, Count);
		Statistics.DeferredDeleteMaxAgeFrames = std::max(
			Statistics.DeferredDeleteMaxAgeFrames, MaxAgeFrames);
	}

	auto FVulkanMemoryBaselineTracker::RecordHeapBudgets(
		std::span<const uint64> UsageBytes,
		std::span<const uint64> BudgetBytes) -> void
	{
		std::lock_guard Lock(Mutex);
		const uint32 HeapCount = static_cast<uint32>(std::min({
			UsageBytes.size(), BudgetBytes.size(),
			static_cast<size_t>(FVulkanMemoryBaselineStatistics::MaxMemoryHeaps)}));
		Statistics.HeapCount = HeapCount;
		for (uint32 HeapIndex = 0; HeapIndex < HeapCount; ++HeapIndex)
		{
			Statistics.HeapUsageBytes[HeapIndex] = UsageBytes[HeapIndex];
			Statistics.HeapBudgetBytes[HeapIndex] = BudgetBytes[HeapIndex];
		}
	}

	auto FVulkanMemoryBaselineTracker::Snapshot() const
		-> FVulkanMemoryBaselineStatistics
	{
		std::lock_guard Lock(Mutex);
		return Statistics;
	}

	auto FVulkanMemoryBaselineTracker::ResetCounters() -> void
	{
		std::lock_guard Lock(Mutex);
		struct FLiveClass
		{
			uint64 AllocationCount = 0;
			uint64 AllocationBytes = 0;
			uint64 ArenaCapacityBytes = 0;
			uint64 ArenaLiveBytes = 0;
		};
		std::array<FLiveClass,
			FVulkanMemoryBaselineStatistics::AllocationClassCount> LiveClasses{};
		for (uint32 ClassIndex = 0; ClassIndex < LiveClasses.size(); ++ClassIndex)
		{
			LiveClasses[ClassIndex] = {
				Statistics.AllocationClasses[ClassIndex].LiveAllocationCount,
				Statistics.AllocationClasses[ClassIndex].LiveBytes,
				Statistics.AllocationClasses[ClassIndex].ArenaCapacityBytes,
				Statistics.AllocationClasses[ClassIndex].ArenaLiveBytes};
		}
		const uint64 DescriptorPoolCount = Statistics.DescriptorPoolCount;
		const uint64 DescriptorPoolSetCapacity =
			Statistics.DescriptorPoolSetCapacity;
		const uint64 DescriptorAllocatedSetCount =
			Statistics.DescriptorAllocatedSetCount;
		const uint64 DeferredDeletePendingCount =
			Statistics.DeferredDeletePendingCount;
		const uint32 HeapCount = Statistics.HeapCount;
		const auto HeapUsageBytes = Statistics.HeapUsageBytes;
		const auto HeapBudgetBytes = Statistics.HeapBudgetBytes;
		Statistics = {};
		for (uint32 ClassIndex = 0; ClassIndex < LiveClasses.size(); ++ClassIndex)
		{
			auto& Class = Statistics.AllocationClasses[ClassIndex];
			Class.LiveAllocationCount = LiveClasses[ClassIndex].AllocationCount;
			Class.LiveBytes = LiveClasses[ClassIndex].AllocationBytes;
			Class.PeakLiveBytes = Class.LiveBytes;
			Class.ArenaCapacityBytes = LiveClasses[ClassIndex].ArenaCapacityBytes;
			Class.ArenaLiveBytes = LiveClasses[ClassIndex].ArenaLiveBytes;
			Class.ArenaHighWaterBytes = Class.ArenaLiveBytes;
		}
		Statistics.DescriptorPoolCount = DescriptorPoolCount;
		Statistics.DescriptorPoolSetCapacity = DescriptorPoolSetCapacity;
		Statistics.DescriptorAllocatedSetCount = DescriptorAllocatedSetCount;
		Statistics.DescriptorPeakAllocatedSetCount = DescriptorAllocatedSetCount;
		Statistics.DeferredDeletePendingCount = DeferredDeletePendingCount;
		Statistics.DeferredDeleteHighWater = DeferredDeletePendingCount;
		Statistics.HeapCount = HeapCount;
		Statistics.HeapUsageBytes = HeapUsageBytes;
		Statistics.HeapBudgetBytes = HeapBudgetBytes;
		CurrentFrameUploadBytes = 0;
		CurrentFrameReadbackBytes = 0;
	}

	auto GetVulkanMemoryBaselineStatistics()
		-> FVulkanMemoryBaselineStatistics
	{
		return GVulkanMemoryBaselineTracker.Snapshot();
	}

	auto ResetVulkanMemoryBaselineStatistics() -> void
	{
		GVulkanMemoryBaselineTracker.ResetCounters();
	}

	auto FormatVulkanMemoryBaselineStatistics(
		const FVulkanMemoryBaselineStatistics& Statistics) -> std::string
	{
		std::string Classes;
		std::string Arenas;
		for (uint32 ClassIndex = 0;
			ClassIndex < FVulkanMemoryBaselineStatistics::AllocationClassCount;
			++ClassIndex)
		{
			const auto& Class = Statistics.AllocationClasses[ClassIndex];
			std::string Buckets;
			for (uint32 BucketIndex = 0;
				BucketIndex < FVulkanAllocationCandidateStatistics::SizeBucketCount;
				++BucketIndex)
			{
				Buckets += std::format("{}{}", BucketIndex == 0 ? "" : ".",
					Class.SizeBuckets[BucketIndex]);
			}
			Classes += std::format("{}{}:{}/{}/{}:[{}]",
				ClassIndex == 0 ? "" : ",", ClassIndex, Class.AllocationCount,
				Class.RequestedBytes, Class.PeakAllocationBytes, Buckets);
			Arenas += std::format("{}{}:{}/{}/{}/{}/{}/{}/{}",
				ClassIndex == 0 ? "" : ",", ClassIndex,
				Class.ArenaCapacityBytes, Class.ArenaLiveBytes,
				Class.ArenaHighWaterBytes, Class.ArenaReuseCount,
				Class.ArenaOverflowCount, Class.ArenaOversizeCount,
				Class.ArenaWaitCount);
		}
		std::string Heaps;
		for (uint32 HeapIndex = 0; HeapIndex < Statistics.HeapCount; ++HeapIndex)
		{
			Heaps += std::format("{}{}:{}/{}", HeapIndex == 0 ? "" : ",",
				HeapIndex, Statistics.HeapUsageBytes[HeapIndex],
				Statistics.HeapBudgetBytes[HeapIndex]);
		}
		return std::format(
			"classes[count/requested/peak:buckets4KiBx2]=[{}] "
			"arena[capacity/live/peak/reuse/overflow/oversize/wait]=[{}] "
			"upload[ops/bytes/framePeak]={}/{}/{} "
			"readback[ops/bytes/framePeak]={}/{}/{} frameWait[count/ns]={}/{} "
			"command[alloc/reuse]={}/{} descriptor[pools/cap/live/peak]={}/{}/{}/{} "
			"delete[pending/peak/released/maxAge]={}/{}/{}/{} heaps[usage/budget]=[{}]",
			Classes, Arenas, Statistics.UploadOperationCount, Statistics.UploadBytes,
			Statistics.PeakUploadBytesPerFrame, Statistics.ReadbackOperationCount,
			Statistics.ReadbackBytes, Statistics.PeakReadbackBytesPerFrame,
			Statistics.FrameFenceWaitCount, Statistics.FrameFenceWaitNanoseconds,
			Statistics.CommandBufferAllocationCount,
			Statistics.CommandBufferReuseCount, Statistics.DescriptorPoolCount,
			Statistics.DescriptorPoolSetCapacity,
			Statistics.DescriptorAllocatedSetCount,
			Statistics.DescriptorPeakAllocatedSetCount,
			Statistics.DeferredDeletePendingCount,
			Statistics.DeferredDeleteHighWater,
			Statistics.DeferredDeleteReleasedCount,
			Statistics.DeferredDeleteMaxAgeFrames, Heaps);
	}

	auto GetRHIMemoryStatistics() -> FRHIMemoryStatistics
	{
		const FVulkanMemoryBaselineStatistics Source =
			GVulkanMemoryBaselineTracker.Snapshot();
		FRHIMemoryStatistics Result;
		for (uint32 ClassIndex = 0; ClassIndex < Result.Classes.size(); ++ClassIndex)
		{
			const auto& Input = Source.AllocationClasses[ClassIndex];
			auto& Output = Result.Classes[ClassIndex];
			Output.LiveAllocationCount = Input.LiveAllocationCount;
			Output.LiveBytes = Input.LiveBytes;
			Output.PeakLiveBytes = Input.PeakLiveBytes;
			Output.AllocationCount = Input.AllocationCount;
			Output.AllocationBytes = Input.ActualBytes;
			Output.AllocationFailureCount = Input.AllocationFailureCount;
			Output.DedicatedAllocationCount = Input.DedicatedAllocationCount;
			Output.PeakAllocationBytes = Input.PeakAllocationBytes;
			Output.ArenaCapacityBytes = Input.ArenaCapacityBytes;
			Output.ArenaLiveBytes = Input.ArenaLiveBytes;
			Output.ArenaHighWaterBytes = Input.ArenaHighWaterBytes;
			Output.ArenaReuseCount = Input.ArenaReuseCount;
			Output.ArenaOverflowCount = Input.ArenaOverflowCount;
			Output.ArenaOversizeCount = Input.ArenaOversizeCount;
			Output.ArenaWaitCount = Input.ArenaWaitCount;
		}
		Result.HeapCount = Source.HeapCount;
		for (uint32 HeapIndex = 0; HeapIndex < Result.HeapCount; ++HeapIndex)
		{
			Result.Heaps[HeapIndex] = {
				.UsageBytes = Source.HeapUsageBytes[HeapIndex],
				.BudgetBytes = Source.HeapBudgetBytes[HeapIndex]};
		}
		Result.UploadOperationCount = Source.UploadOperationCount;
		Result.UploadBytes = Source.UploadBytes;
		Result.PeakUploadBytesPerFrame = Source.PeakUploadBytesPerFrame;
		Result.ReadbackOperationCount = Source.ReadbackOperationCount;
		Result.ReadbackBytes = Source.ReadbackBytes;
		Result.PeakReadbackBytesPerFrame = Source.PeakReadbackBytesPerFrame;
		Result.GPUWaitCount = Source.FrameFenceWaitCount;
		Result.GPUWaitNanoseconds = Source.FrameFenceWaitNanoseconds;
		Result.RetirementPendingCount = Source.DeferredDeletePendingCount;
		Result.RetirementHighWater = Source.DeferredDeleteHighWater;
		Result.RetirementReleasedCount = Source.DeferredDeleteReleasedCount;
		Result.RetirementMaxTokenLag = Source.DeferredDeleteMaxAgeFrames;
		return Result;
	}
} // namespace Durin::VulkanRHI
