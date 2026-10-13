#include "VulkanCreationTiming.h"

#if DURIN_VULKAN_TEST_SUPPORT
#include "Threading/RunnableThread.h"
#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		std::atomic<bool> GCreationTimingEnabled = false;
		std::atomic<uint64> GCreationTimingId = 0;
		std::mutex GCreationTimingMutex;
		std::vector<FVulkanCreationTiming> GCreationTimings;
		size_t GCreationTimingCapacity = 0;
		uint64 GCreationTimingDropped = 0;
		thread_local FVulkanCreationTiming* GActiveCreationTiming = nullptr;
	}

	auto GetVulkanDeviceDescriptionForTiming() -> std::string
	{
		const auto& Props = FVulkanDynamicRHI::Get().GetDeviceForTesting()->GetGpuProperties();
		FRHIDiagnosticAvailability Availability;
		auto CaptureAvailability = [&] { Availability = GDynamicRHI->RHIGetDiagnosticSnapshot().Availability; };
		if (GRHIThread && !IsInRHIThread())
			GCommandListExecutor.ExecuteSynchronousOperation(false, CaptureAvailability);
		else CaptureAvailability();
		return std::format("gpu={}\nvendor={}\ndevice={}\ndriver={}\napi={}\nvalidation_active={}\ndebug_utils_active={}\n",
			Props.deviceName.data(), Props.vendorID, Props.deviceID, Props.driverVersion, Props.apiVersion,
			Availability.bValidationLayerActive, Availability.bDebugUtilsActive);
	}

	auto GetVulkanDeviceLuidForTiming() -> std::optional<std::array<std::byte, 8>>
	{
		const auto* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		if (!Device) return std::nullopt;
		const auto Properties = Device->GetGpu().getProperties2<vk::PhysicalDeviceProperties2,
			vk::PhysicalDeviceIDProperties>();
		const auto& Id = Properties.get<vk::PhysicalDeviceIDProperties>();
		if (!Id.deviceLUIDValid) return std::nullopt;
		std::array<std::byte, 8> Result;
		static_assert(Result.size() == VK_LUID_SIZE);
		std::memcpy(Result.data(), Id.deviceLUID.data(), Result.size());
		return Result;
	}

	auto VulkanCreationTimestamp() -> uint64
	{
		return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	auto BeginVulkanCreationTimingCapture(size_t Capacity) -> void
	{
		std::lock_guard Lock(GCreationTimingMutex);
		require(!GCreationTimingEnabled.load());
		require(Capacity > 0 && Capacity <= 65536);
		GCreationTimings.clear();
		GCreationTimings.reserve(Capacity);
		GCreationTimingCapacity = Capacity;
		GCreationTimingDropped = 0;
		GCreationTimingEnabled.store(true, std::memory_order_release);
	}

	auto EndVulkanCreationTimingCapture(uint64& Dropped)
		-> std::vector<FVulkanCreationTiming>
	{
		GCreationTimingEnabled.store(false, std::memory_order_release);
		std::lock_guard Lock(GCreationTimingMutex);
		Dropped = GCreationTimingDropped;
		return std::move(GCreationTimings);
	}

	FVulkanCreationTimingScope::FVulkanCreationTimingScope(EVulkanCreationKind Kind)
		: Previous(GActiveCreationTiming), bEnabled(GCreationTimingEnabled.load(std::memory_order_acquire))
	{
		if (!bEnabled) return;
		Timing.Entry = VulkanCreationTimestamp();
		Timing.RequestId = GCreationTimingId.fetch_add(1, std::memory_order_relaxed) + 1;
		Timing.Kind = Kind;
		Timing.bCompute = Kind == EVulkanCreationKind::ComputePipeline;
		GActiveCreationTiming = &Timing;
	}

	FVulkanCreationTimingScope::~FVulkanCreationTimingScope()
	{
		if (!bEnabled) return;
		Timing.Returned = VulkanCreationTimestamp();
		GActiveCreationTiming = Previous;
		std::lock_guard Lock(GCreationTimingMutex);
		if (GCreationTimings.size() < GCreationTimingCapacity)
			GCreationTimings.push_back(Timing);
		else ++GCreationTimingDropped;
	}

	auto GetActiveVulkanCreationTiming() -> FVulkanCreationTiming*
	{
		return GActiveCreationTiming;
	}

	FVulkanCreationBodyTimingScope::FVulkanCreationBodyTimingScope(FVulkanCreationTiming* Timing)
		: Previous(GActiveCreationTiming)
	{
		GActiveCreationTiming = Timing;
		if (Timing) Timing->BodyStart = VulkanCreationTimestamp();
	}

	FVulkanCreationBodyTimingScope::~FVulkanCreationBodyTimingScope()
	{
		if (GActiveCreationTiming) GActiveCreationTiming->BodyEnd = VulkanCreationTimestamp();
		GActiveCreationTiming = Previous;
	}

	FVulkanNativeCreationTimingScope::FVulkanNativeCreationTimingScope()
	{
		if (GActiveCreationTiming)
		{
			Start = VulkanCreationTimestamp();
			if (!GActiveCreationTiming->NativeStart) GActiveCreationTiming->NativeStart = Start;
		}
	}

	FVulkanNativeCreationTimingScope::~FVulkanNativeCreationTimingScope()
	{
		if (Start)
		{
			GActiveCreationTiming->NativeEnd = VulkanCreationTimestamp();
			GActiveCreationTiming->NativeNanoseconds += GActiveCreationTiming->NativeEnd - Start;
		}
	}

	FVulkanCreationDependencyTimingScope::FVulkanCreationDependencyTimingScope()
	{
		if (GActiveCreationTiming) Start = VulkanCreationTimestamp();
	}

	FVulkanCreationDependencyTimingScope::~FVulkanCreationDependencyTimingScope()
	{
		if (Start) GActiveCreationTiming->DependencyNanoseconds += VulkanCreationTimestamp() - Start;
	}

	FVulkanCreationCpuTimingScope::FVulkanCreationCpuTimingScope(bool bInNormalization)
		: bNormalization(bInNormalization)
	{
		if (GActiveCreationTiming)
		{
			Start = VulkanCreationTimestamp();
			ExcludedBefore = GActiveCreationTiming->NativeNanoseconds + GActiveCreationTiming->DependencyNanoseconds;
		}
	}

	FVulkanCreationCpuTimingScope::~FVulkanCreationCpuTimingScope() { Finish(); }

	auto FVulkanCreationCpuTimingScope::Finish() -> void
	{
		if (!Start) return;
		const uint64 Elapsed = VulkanCreationTimestamp() - Start;
		const uint64 Excluded = GActiveCreationTiming->NativeNanoseconds
			+ GActiveCreationTiming->DependencyNanoseconds - ExcludedBefore;
		require(Elapsed >= Excluded);
		(bNormalization ? GActiveCreationTiming->NormalizationNanoseconds
			: GActiveCreationTiming->PreparationNanoseconds) += Elapsed - Excluded;
		Start = 0;
	}
} // namespace Durin::VulkanRHI
#endif
