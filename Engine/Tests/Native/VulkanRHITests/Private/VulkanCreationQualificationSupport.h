#pragma once

#include "VulkanCreationTiming.h"
#include "PCH.VulkanRHI.h"
#include "VulkanExtensions.h"
#include <fstream>
#ifdef _WIN32
#include "Windows/WindowsPlatform.h"
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>
#include <intrin.h>
#endif

namespace Durin::VulkanRHI
{
	// Bootstrap on the test main thread before measurement. Retain the loader-selected
	// layer until process exit so allocator state survives repeated RHI owner threads.
	inline auto PrepareCreationQualificationValidationLayer() -> bool
	{
#ifdef _WIN32
		if (!ResolveVulkanValidationPolicy(std::getenv("DURIN_VULKAN_VALIDATION"),
			DURIN_BUILD_DEBUG != 0, DURIN_BUILD_SHIPPING != 0).bRequestDiagnostics) return true;
		static const bool Prepared = [] {
			const HMODULE Loader = LoadLibraryW(L"vulkan-1.dll");
			if (!Loader) return false;
			const auto Create = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(Loader, "vkCreateInstance"));
			const auto Destroy = reinterpret_cast<PFN_vkDestroyInstance>(GetProcAddress(Loader, "vkDestroyInstance"));
			if (!Create || !Destroy) { FreeLibrary(Loader); return false; }
			const char* LayerName = "VK_LAYER_KHRONOS_validation";
			VkInstanceCreateInfo Info{};
			Info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			Info.enabledLayerCount = 1;
			Info.ppEnabledLayerNames = &LayerName;
			VkInstance Instance = VK_NULL_HANDLE;
			const VkResult Result = Create(&Info, nullptr, &Instance);
			HMODULE Layer = nullptr;
			if (Result == VK_SUCCESS)
			{
				// Intentionally hold one reference until OS process teardown.
				GetModuleHandleExW(0, L"VkLayer_khronos_validation.dll", &Layer);
				Destroy(Instance, nullptr);
			}
			FreeLibrary(Loader);
			return (Result == VK_SUCCESS && Layer) || Result == VK_ERROR_LAYER_NOT_PRESENT;
		}();
		return Prepared;
#else
		return true;
#endif
	}

	// Optional process-local qualification setting, identical in baseline and refactor runs.
	inline auto ConfigureCreationQualificationAffinity() -> bool
	{
		const char* Requested = std::getenv("DURIN_CREATION_QUALIFICATION_AFFINITY");
		if (!Requested) return true;
#ifdef _WIN32
		char* End = nullptr;
		const auto Mask = std::strtoull(Requested, &End, 0);
		DWORD_PTR ProcessMask = 0, SystemMask = 0;
		if (!Mask || !End || *End || !GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask)
			|| (Mask & SystemMask) != Mask) return false;
		return SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(Mask)) != 0;
#else
		return false;
#endif
	}

	inline auto ConfigureCreationQualificationHighQos() -> bool
	{
		const char* Requested = std::getenv("DURIN_CREATION_QUALIFICATION_HIGH_QOS");
		if (!Requested) return true;
		if (std::string_view(Requested) != "1") return false;
#ifdef _WIN32
		PROCESS_POWER_THROTTLING_STATE State{};
		State.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		State.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
		// Explicit HighQoS; does not modify the system power plan or scheduling priority.
		State.StateMask = 0;
		return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &State, sizeof(State)) != 0;
#else
		return false;
#endif
	}

	// Sampled per-window peaks, distinct from Windows' process-lifetime commit high-water mark.
	struct FCreationMemorySamples
	{
		bool bAvailable = false;
		uint64 SampleCount = 0;
		uint64 MaxSampleGapNanoseconds = 0;
		uint64 PrivateBytesPeak = 0;
		uint64 ProcessLifetimeCommitPeak = 0;
		uint64 LocalVideoBytesPeak = 0;
		uint64 NonLocalVideoBytesPeak = 0;
	};

	// Joins its 1 ms observer before the associated device can be destroyed.
	class FCreationMemorySampler
	{
	public:
		FCreationMemorySampler()
		{
#ifdef _WIN32
			const auto Id = GetVulkanDeviceLuidForTiming();
			Microsoft::WRL::ComPtr<IDXGIFactory4> Factory;
			if (Id && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&Factory))))
			{
				LUID Luid{};
				static_assert(sizeof(Luid) == 8);
				std::memcpy(&Luid, Id->data(), sizeof(Luid));
				Factory->EnumAdapterByLuid(Luid, IID_PPV_ARGS(&Adapter));
			}
			Samples.bAvailable = Adapter != nullptr;
#endif
			Sample();
			Worker = std::jthread([this](std::stop_token Token) {
				while (!Token.stop_requested())
				{
					Sample();
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			});
		}
		~FCreationMemorySampler() { Stop(); }
		auto Stop() -> FCreationMemorySamples
		{
			if (Worker.joinable())
			{
				Worker.request_stop();
				Worker.join();
			}
			if (!bStopped) { Sample(); bStopped = true; }
			return Samples;
		}
	private:
		auto Sample() -> void
		{
			const uint64 Now = VulkanCreationTimestamp();
			if (LastSample) Samples.MaxSampleGapNanoseconds =
				std::max(Samples.MaxSampleGapNanoseconds, Now - LastSample);
			LastSample = Now;
			++Samples.SampleCount;
#ifdef _WIN32
			PROCESS_MEMORY_COUNTERS_EX Process{};
			Process.cb = sizeof(Process);
			if (K32GetProcessMemoryInfo(GetCurrentProcess(),
				reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Process), sizeof(Process)))
			{
				Samples.PrivateBytesPeak = std::max(Samples.PrivateBytesPeak, uint64(Process.PrivateUsage));
				Samples.ProcessLifetimeCommitPeak = std::max(Samples.ProcessLifetimeCommitPeak,
					uint64(Process.PeakPagefileUsage));
			}
			else Samples.bAvailable = false;
			if (Adapter)
			{
				DXGI_QUERY_VIDEO_MEMORY_INFO Local{}, NonLocal{};
				if (SUCCEEDED(Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Local))
					&& SUCCEEDED(Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &NonLocal)))
				{
					Samples.LocalVideoBytesPeak = std::max(Samples.LocalVideoBytesPeak, Local.CurrentUsage);
					Samples.NonLocalVideoBytesPeak = std::max(Samples.NonLocalVideoBytesPeak, NonLocal.CurrentUsage);
				}
				else Samples.bAvailable = false;
			}
#endif
		}
		FCreationMemorySamples Samples;
		bool bStopped = false;
		uint64 LastSample = 0;
#ifdef _WIN32
		Microsoft::WRL::ComPtr<IDXGIAdapter3> Adapter;
#endif
		std::jthread Worker;
	};

	inline auto WriteCreationMemorySample(std::ostream& Stream, const FCreationMemorySamples& Sample) -> void
	{
		Stream << ',' << Sample.bAvailable << ',' << Sample.SampleCount << ','
			<< Sample.MaxSampleGapNanoseconds << ',' << Sample.PrivateBytesPeak << ','
			<< Sample.ProcessLifetimeCommitPeak << ',' << Sample.LocalVideoBytesPeak << ','
			<< Sample.NonLocalVideoBytesPeak;
	}
	inline constexpr std::string_view CreationMemoryColumns =
		",memory_available,memory_samples,max_sample_gap_ns,private_peak_bytes,process_lifetime_commit_peak_bytes,local_video_peak_bytes,nonlocal_video_peak_bytes";

	inline auto WriteCreationHost(std::ostream& Stream) -> void
	{
		Stream << "build_debug=" << DURIN_BUILD_DEBUG << " build_shipping=" << DURIN_BUILD_SHIPPING << '\n';
		for (const char* Name : {"DURIN_RHI_EXECUTION", "DURIN_VULKAN_VALIDATION", "VK_LAYER_DISABLES"})
		{
			const char* Value = std::getenv(Name);
			Stream << Name << '=' << (Value ? Value : "<unset>") << '\n';
		}
#ifdef _WIN32
		Stream << "validation_layer_lifetime=main_thread_bootstrap_process_if_available\n";
		DWORD_PTR ProcessMask = 0, SystemMask = 0;
		if (GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask))
			Stream << "process_affinity=" << ProcessMask << '\n';
		PROCESS_POWER_THROTTLING_STATE PowerState{};
		PowerState.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		if (GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &PowerState, sizeof(PowerState)))
			Stream << "process_power_control=" << PowerState.ControlMask
				<< " process_power_state=" << PowerState.StateMask << '\n';
		MEMORYSTATUSEX Memory{};
		Memory.dwLength = sizeof(Memory);
		if (GlobalMemoryStatusEx(&Memory)) Stream << "ram_bytes=" << Memory.ullTotalPhys << '\n';
		std::array<int, 12> Brand{};
		for (int Index = 0; Index < 3; ++Index) __cpuid(Brand.data() + Index * 4, 0x80000002 + Index);
		std::string BrandName(reinterpret_cast<const char*>(Brand.data()), 48);
		if (const auto End = BrandName.find('\0'); End != std::string::npos) BrandName.resize(End);
		Stream << "cpu=" << BrandName << '\n';
#endif
	}

	inline constexpr std::string_view CreationRequestColumns =
		"request,key,compute,success,entry_ns,scheduled_ns,body_start_ns,native_start_ns,native_end_ns,body_end_ns,returned_ns,kind,native_ns,dependency_ns,admitted_ns,wait_begin_ns,wait_end_ns,normalization_ns,preparation_ns,background";
	inline auto WriteCreationRequest(std::ostream& Stream, const FVulkanCreationTiming& S) -> void
	{
		Stream << S.RequestId << ',' << S.KeyHash << ',' << S.bCompute << ',' << S.bSucceeded << ','
			<< S.Entry << ',' << S.Scheduled << ',' << S.BodyStart << ',' << S.NativeStart << ','
			<< S.NativeEnd << ',' << S.BodyEnd << ',' << S.Returned << ',' << uint32(S.Kind) << ','
			<< S.NativeNanoseconds << ',' << S.DependencyNanoseconds << ',' << S.Scheduling.Admitted
			<< ',' << S.Scheduling.WaitBegin << ',' << S.Scheduling.WaitEnd
			<< ',' << S.NormalizationNanoseconds << ',' << S.PreparationNanoseconds << ',' << S.bBackground;
	}
}
