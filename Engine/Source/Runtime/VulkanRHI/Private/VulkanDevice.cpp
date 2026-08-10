#include "VulkanDevice.h"
#include "VulkanCompletion.h"
#include "VulkanDiagnostics.h"

#include "RHICommandList.h"
#include "Threading/RunnableThread.h"
#include "VulkanDynamicRHI.h"
#include "VulkanExtensions.h"
#include "VulkanContext.h"
#include "VulkanRenderPass.h"
#include "VulkanPipeline.h"
#include "VulkanQueue.h"
#include "VulkanSubmission.h"
#include "VulkanTransferArena.h"
#include "VulkanDescriptorSets.h"
#include "VulkanBuffer.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto HasExtension(const std::vector<std::string>& Extensions, std::string_view Name) -> bool
		{
			return std::ranges::any_of(Extensions,
				[Name](const std::string& Extension) { return Extension == Name; });
		}

		auto DeviceTypePreference(vk::PhysicalDeviceType Type) -> uint32
		{
			switch (Type)
			{
			case vk::PhysicalDeviceType::eDiscreteGpu: return 0;
			case vk::PhysicalDeviceType::eIntegratedGpu: return 1;
			case vk::PhysicalDeviceType::eVirtualGpu: return 2;
			case vk::PhysicalDeviceType::eOther: return 3;
			case vk::PhysicalDeviceType::eCpu: return 4;
			default: return 5;
			}
		}
	}

	auto EvaluateVulkanPhysicalDeviceCandidate(
		const FVulkanPhysicalDeviceCandidateInput& Input)
		-> FVulkanPhysicalDeviceCandidateEvaluation
	{
		FVulkanPhysicalDeviceCandidateEvaluation Result;
		if (Input.ApiVersion < VK_API_VERSION_1_1)
			Result.RejectionReasons.emplace_back("device API version is below Vulkan 1.1");
		if (!HasExtension(Input.AvailableExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
			Result.RejectionReasons.emplace_back("missing platform required extension VK_KHR_swapchain");
		if (!Input.bFillModeNonSolid)
			Result.RejectionReasons.emplace_back("missing required fillModeNonSolid feature");
		if (!Input.bShaderDrawParameters)
			Result.RejectionReasons.emplace_back("missing required shaderDrawParameters feature");
		if (Input.MaxImageDimension2D == 0)
			Result.RejectionReasons.emplace_back("maxImageDimension2D is zero");
		if (Input.MaxImageDimensionCube == 0)
			Result.RejectionReasons.emplace_back("maxImageDimensionCube is zero");
		if (Input.MaxImageArrayLayers < TextureCubeFaceCount)
			Result.RejectionReasons.emplace_back("maxImageArrayLayers is below six");
		for (uint32 Index = 0; Index < Input.QueueFamilies.size(); ++Index)
		{
			const FVulkanQueueFamilyCandidate& Queue = Input.QueueFamilies[Index];
			const vk::QueueFlags Required = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
			if (Queue.QueueCount > 0 && (Queue.Flags & Required) == Required
				&& Queue.bSupportsWin32Presentation)
			{
				Result.GraphicsPresentQueueFamilyIndex = static_cast<int32>(Index);
				break;
			}
		}
		if (Result.GraphicsPresentQueueFamilyIndex < 0)
			Result.RejectionReasons.emplace_back(
				"no queue family provides graphics, compute, and Win32 presentation");
		if (!Result.IsSuitable()) return Result;

		Result.EnabledExtensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		const bool bSynchronization2Extension = HasExtension(
			Input.AvailableExtensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
		Result.bEnableSynchronization2 = Input.bSynchronization2Feature
			&& (Input.ApiVersion >= VK_API_VERSION_1_3 || bSynchronization2Extension);
		if (Result.bEnableSynchronization2 && Input.ApiVersion < VK_API_VERSION_1_3)
			Result.EnabledExtensions.emplace_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
		Result.bEnableSwapchainMaintenance1 = Input.bHasSwapchainMaintenanceInstanceDependencies
			&& Input.bSwapchainMaintenanceFeature
			&& HasExtension(Input.AvailableExtensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
		if (Result.bEnableSwapchainMaintenance1)
			Result.EnabledExtensions.emplace_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
		return Result;
	}

	auto IsVulkanPhysicalDeviceCandidatePreferred(
		const FVulkanPhysicalDeviceCandidateInput& Left,
		const FVulkanPhysicalDeviceCandidateInput& Right) -> bool
	{
		return std::tuple{
			DeviceTypePreference(Left.DeviceType),
			std::numeric_limits<uint32>::max() - Left.MaxImageDimension2D,
			std::numeric_limits<uint32>::max() - Left.ApiVersion,
			Left.VendorId, Left.DeviceId, Left.DeviceName}
			< std::tuple{
				DeviceTypePreference(Right.DeviceType),
				std::numeric_limits<uint32>::max() - Right.MaxImageDimension2D,
				std::numeric_limits<uint32>::max() - Right.ApiVersion,
				Right.VendorId, Right.DeviceId, Right.DeviceName};
	}

	auto FormatVulkanPhysicalDeviceRejectionDiagnostic(
		std::span<const FVulkanPhysicalDeviceCandidateInput> Inputs,
		std::span<const FVulkanPhysicalDeviceCandidateEvaluation> Evaluations)
		-> std::string
	{
		check(Inputs.size() == Evaluations.size());
		std::string Diagnostic = "Vulkan physical-device selection failed:";
		const size_t DeviceCount = std::min<size_t>(Inputs.size(), 16);
		for (size_t DeviceIndex = 0; DeviceIndex < DeviceCount; ++DeviceIndex)
		{
			Diagnostic += std::format(" [{}] {}:", DeviceIndex,
				Inputs[DeviceIndex].DeviceName.substr(0, 256));
			const auto& Reasons = Evaluations[DeviceIndex].RejectionReasons;
			const size_t ReasonCount = std::min<size_t>(Reasons.size(), 8);
			for (size_t ReasonIndex = 0; ReasonIndex < ReasonCount; ++ReasonIndex)
				Diagnostic += std::format(" {}", Reasons[ReasonIndex].substr(0, 256));
			if (Reasons.size() > ReasonCount)
				Diagnostic += std::format(" (+{} reasons)", Reasons.size() - ReasonCount);
		}
		if (Inputs.size() > DeviceCount)
			Diagnostic += std::format(" (+{} devices)", Inputs.size() - DeviceCount);
		return Diagnostic;
	}

	FDeferredDeletionQueue::FDeferredDeletionQueue(FVulkanDevice* InDevice)
		: Device(InDevice)
	{
	}

	auto FDeferredDeletionQueue::ReleaseResources(bool bDeleteImmediately) -> void
	{
		CheckVulkanRHIThread();
		if (bDeleteImmediately)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			uint64 MaxTokenLag = 0;
			for (const FEntry& Entry : Entries)
			{
				MaxTokenLag = std::max(MaxTokenLag,
					Device->GetCompletionTracker().GetLastSubmittedToken()
						- std::min(Entry.CompletionToken,
							Device->GetCompletionTracker().GetLastSubmittedToken()));
			}
			GVulkanMemoryBaselineTracker.RecordDeferredDeletesReleased(
				Entries.size(), MaxTokenLag);
			ReleaseResourceImmediately(Entries);
			Entries.clear();
		}
		else
		{
			std::vector<FEntry> EntriesToDelete;
			{
				std::lock_guard<std::mutex> Lock(Mutex);

				const uint64 CompletedToken =
					Device->GetCompletionTracker().GetCompletedToken();
				auto DeleteSubRange = std::ranges::partition(Entries, [&](const FEntry& Entry) {
					return Entry.CompletionToken > CompletedToken;
				});

				if (DeleteSubRange.begin() != Entries.end())
				{
					EntriesToDelete.insert(EntriesToDelete.end(), std::make_move_iterator(DeleteSubRange.begin()), std::make_move_iterator(DeleteSubRange.end()));
					Entries.erase(DeleteSubRange.begin(), DeleteSubRange.end());
				}
			}

			if (!EntriesToDelete.empty())
			{
				uint64 MaxTokenLag = 0;
				for (const FEntry& Entry : EntriesToDelete)
				{
					MaxTokenLag = std::max(MaxTokenLag,
						Device->GetCompletionTracker().GetCompletedToken()
							- Entry.CompletionToken);
				}
				GVulkanMemoryBaselineTracker.RecordDeferredDeletesReleased(
					EntriesToDelete.size(), MaxTokenLag);
				ReleaseResourceImmediately(EntriesToDelete);
			}
		}
	}

	auto FDeferredDeletionQueue::Clear() -> void
	{
		ReleaseResources(true);
	}

	auto FDeferredDeletionQueue::EnqueueGenericResource(EType Type, uint64 Handle) -> void
	{
		const uint64 CompletionToken =
			Device->GetCompletionTracker().GetLastReservedToken();
		std::lock_guard<std::mutex> Lock(Mutex);
		Entries.emplace_back(Type, CompletionToken, Handle);
		GVulkanMemoryBaselineTracker.RecordDeferredDeleteEnqueued();
	}

	auto FDeferredDeletionQueue::EnqueueAllocatedResource(EType Type, uint64 Handle, const FVulkanAllocation& Allocation) -> void
	{
		const uint64 CompletionToken =
			Device->GetCompletionTracker().GetLastReservedToken();
		std::lock_guard<std::mutex> Lock(Mutex);
		Entries.emplace_back(Type, CompletionToken, Handle, Allocation); // Copy allocation here
		GVulkanMemoryBaselineTracker.RecordDeferredDeleteEnqueued();
	}

#define DURIN_VK_DESTROY_CASE(Type, ...)                                                 \
	case EType::Type:                                                                   \
		__VA_ARGS__                                                                     \
		DeviceHandle.destroy##Type(vk::Type{reinterpret_cast<Vk##Type>(Entry.Handle)}); \
		break

#define DURIN_VMA_DESTROY_CASE(Type, ...)                                                                   \
	case EType::Type:                                                                                      \
		__VA_ARGS__                                                                                        \
		MemoryManager.Destroy##Type(Entry.Allocation, vk::Type{reinterpret_cast<Vk##Type>(Entry.Handle)}); \
		break

	auto FDeferredDeletionQueue::ReleaseResourceImmediately(std::vector<FEntry>& InEntries) const -> void
	{
		const vk::Device DeviceHandle = Device->GetHandle();
		FVulkanMemoryManager& MemoryManager = Device->GetMemoryManager();

		for (FEntry& Entry : InEntries)
		{
			if (Entry.Allocation.IsValid())
			{
				switch (Entry.Type)
				{
					DURIN_VMA_DESTROY_CASE(Image);
					DURIN_VMA_DESTROY_CASE(Buffer);
				default:
					DURIN_ERROR("Failed to release a deferred Vulkan allocation: unsupported resourceType={}.", static_cast<uint32>(Entry.Type));
					break;
				}
			}
			else
			{
				switch (Entry.Type)
				{
					DURIN_VK_DESTROY_CASE(RenderPass);
					DURIN_VK_DESTROY_CASE(Buffer);
					DURIN_VK_DESTROY_CASE(BufferView);
					DURIN_VK_DESTROY_CASE(Image);
					DURIN_VK_DESTROY_CASE(ImageView);
					DURIN_VK_DESTROY_CASE(Pipeline);
					DURIN_VK_DESTROY_CASE(PipelineLayout);
					DURIN_VK_DESTROY_CASE(DescriptorPool);
					DURIN_VK_DESTROY_CASE(Framebuffer);
					DURIN_VK_DESTROY_CASE(DescriptorSetLayout);
					DURIN_VK_DESTROY_CASE(Sampler);
					DURIN_VK_DESTROY_CASE(Semaphore);
					DURIN_VK_DESTROY_CASE(ShaderModule);
					DURIN_VK_DESTROY_CASE(Event);
					// TODO: Others
				default:
					DURIN_ERROR("Failed to release a deferred Vulkan resource: unsupported resourceType={}.", static_cast<uint32>(Entry.Type));
					break;
				}
			}
		}
	}

	FVulkanDevice::FVulkanDevice(
		FVulkanDynamicRHI* InRHI,
		vk::PhysicalDevice InGpu,
		FVulkanPhysicalDeviceCandidateEvaluation InEvaluation)
		: RHI(InRHI)
		, Gpu(InGpu)
		, FenceManager({*this})
		, DeferredDeletionQueue(this)
	{
		check(InEvaluation.IsSuitable());
		GraphicsQueueFamilyIndex = InEvaluation.GraphicsPresentQueueFamilyIndex;
		ComputeQueueFamilyIndex = GraphicsQueueFamilyIndex;
		TransferQueueFamilyIndex = GraphicsQueueFamilyIndex;
		DeviceExtensions = std::move(InEvaluation.EnabledExtensions);
		bSupportsSwapchainMaintenance1 = InEvaluation.bEnableSwapchainMaintenance1;
		bSupportsSynchronization2 = InEvaluation.bEnableSynchronization2;
	}

	FVulkanDevice::~FVulkanDevice()
	{
		Destroy();
	}

	void FVulkanDevice::InitGpu(const uint32 EnabledInstanceExtensionCount)
	{
		CheckVulkanRHIThread();
		GpuProps = Gpu.getProperties();
		DURIN_TRACE("Vulkan physical device: name=\"{}\", type={}, vendor=0x{:04x}, device=0x{:04x}, API={}.{}.{}, driver=0x{:x}.",
			GpuProps.deviceName.data(), vk::to_string(GpuProps.deviceType), GpuProps.vendorID, GpuProps.deviceID,
			vk::apiVersionMajor(GpuProps.apiVersion), vk::apiVersionMinor(GpuProps.apiVersion), vk::apiVersionPatch(GpuProps.apiVersion), GpuProps.driverVersion);

		QueueFamilyProps = Gpu.getQueueFamilyProperties();
		CreateDevice();
		DURIN_INFO("Vulkan initialized: GPU=\"{}\", type={}, API={}.{}.{}, driver=0x{:x}, extensions(instance={}, device={}), queues(graphics={}, compute={} {}, transfer={} {}).",
			GpuProps.deviceName.data(), vk::to_string(GpuProps.deviceType), vk::apiVersionMajor(GpuProps.apiVersion), vk::apiVersionMinor(GpuProps.apiVersion),
			vk::apiVersionPatch(GpuProps.apiVersion), GpuProps.driverVersion, EnabledInstanceExtensionCount, DeviceExtensions.size(), GraphicsQueueFamilyIndex,
			ComputeQueueFamilyIndex, ComputeQueueFamilyIndex == GraphicsQueueFamilyIndex ? "shared" : "separate", TransferQueueFamilyIndex,
			TransferQueueFamilyIndex == GraphicsQueueFamilyIndex || TransferQueueFamilyIndex == ComputeQueueFamilyIndex ? "shared" : "separate");
		MemoryManager.Init(this);
		CompletionTracker = new FVulkanCompletionTracker(*this);
		UploadArena = new FVulkanTransferArena(*this, {
			.AllocationClass = EVulkanAllocationClassCandidate::TransferUpload,
			.PageSize = 8ull * 1024 * 1024,
			.MaxPageCount = 4,
			.DebugName = "VulkanUploadArena"});
		ReadbackArena = new FVulkanTransferArena(*this, {
			.AllocationClass = EVulkanAllocationClassCandidate::TransferReadback,
			.PageSize = 4ull * 1024 * 1024,
			.MaxPageCount = 2,
			.DebugName = "VulkanReadbackArena"});

		ImmediateContext = new FVulkanCommandListContext(RHI, *this, GraphicsQueue);

		RenderPassManager = new FVulkanRenderPassManager(*this);
		PipelineManager = new FVulkanPipelineManager(*this);
		DescriptorSetCache = new FVulkanDescriptorSetLayoutCache(*this);
		GlobalDescriptorPool = new FVulkanGlobalDescriptorPool(*this);
		DynamicUniformBufferAllocator = new FVulkanDynamicUniformBufferAllocator(*this);
		DynamicStorageBufferAllocator = new FVulkanDynamicStorageBufferAllocator(*this);
		GraphicsCacheStatistics.DescriptorSnapshots.Capacity = 512;
		GraphicsCacheStatistics.DescriptorValueCapacity = 8192;
		GraphicsCacheStatistics.StructuralLayouts.Capacity = 256;
		GraphicsCacheStatistics.GraphicsPipelines.Capacity = 2048;

		for (auto& Frame : Frames)
		{
			Frame = new FVulkanFrame(*this);
		}
	}

	auto FVulkanDevice::ResetGraphicsCacheStatistics() -> void
	{
		const uint64 DescriptorOccupancy = GraphicsCacheStatistics.DescriptorSnapshots.Occupancy;
		const uint64 DescriptorValueOccupancy = GraphicsCacheStatistics.DescriptorValueOccupancy;
		const uint64 LayoutOccupancy = GraphicsCacheStatistics.StructuralLayouts.Occupancy;
		const uint64 PipelineOccupancy = GraphicsCacheStatistics.GraphicsPipelines.Occupancy;
		GraphicsCacheStatistics = {};
		GraphicsCacheStatistics.DescriptorSnapshots.Capacity = 512;
		GraphicsCacheStatistics.DescriptorSnapshots.Occupancy = DescriptorOccupancy;
		GraphicsCacheStatistics.DescriptorValueCapacity = 8192;
		GraphicsCacheStatistics.DescriptorValueOccupancy = DescriptorValueOccupancy;
		GraphicsCacheStatistics.StructuralLayouts.Capacity = 256;
		GraphicsCacheStatistics.StructuralLayouts.Occupancy = LayoutOccupancy;
		GraphicsCacheStatistics.GraphicsPipelines.Capacity = 2048;
		GraphicsCacheStatistics.GraphicsPipelines.Occupancy = PipelineOccupancy;
	}

	auto FVulkanDevice::CreateDevice() -> void
	{
		assert(Device == VK_NULL_HANDLE);
		const float QueuePriority = 1.0f;
		vk::DeviceQueueCreateInfo QueueCreateInfo;
		QueueCreateInfo.setQueueFamilyIndex(GraphicsQueueFamilyIndex)
			.setQueueCount(1)
			.setPQueuePriorities(&QueuePriority);

		vk::PhysicalDeviceFeatures DeviceFeatures;
		const vk::PhysicalDeviceFeatures AvailableFeatures = Gpu.getFeatures();
		DeviceFeatures.fillModeNonSolid = AvailableFeatures.fillModeNonSolid;
		DeviceFeatures.depthClamp = AvailableFeatures.depthClamp;
		DeviceFeatures.wideLines = AvailableFeatures.wideLines;
		vk::DeviceCreateInfo DeviceInfo;
		vk::PhysicalDeviceVulkan11Features Vulkan11Features;
		vk::PhysicalDeviceVulkan13Features Vulkan13Features;
		vk::PhysicalDeviceSynchronization2FeaturesKHR Synchronization2Features;
		vk::PhysicalDeviceSwapchainMaintenance1FeaturesEXT SwapchainMaintenanceFeatures;
		std::vector<const char*> DeviceExtensionNames;
		for (const std::string& Extension : DeviceExtensions)
			DeviceExtensionNames.push_back(Extension.c_str());
		DeviceInfo.setQueueCreateInfos(QueueCreateInfo);
		DeviceInfo.setPEnabledFeatures(&DeviceFeatures);
		DeviceInfo.setEnabledExtensionCount(static_cast<uint32>(DeviceExtensionNames.size()));
		DeviceInfo.setPpEnabledExtensionNames(DeviceExtensionNames.data());
		Vulkan11Features.shaderDrawParameters = vk::True;
		DeviceInfo.setPNext(&Vulkan11Features);
		void* FeatureTail = &Vulkan11Features;
		if (bSupportsSynchronization2)
		{
			if (GpuProps.apiVersion >= VK_API_VERSION_1_3)
			{
				Vulkan13Features.synchronization2 = vk::True;
				Vulkan11Features.setPNext(&Vulkan13Features);
				FeatureTail = &Vulkan13Features;
			}
			else
			{
				Synchronization2Features.synchronization2 = vk::True;
				Vulkan11Features.setPNext(&Synchronization2Features);
				FeatureTail = &Synchronization2Features;
			}
		}
		if (bSupportsSwapchainMaintenance1)
		{
			SwapchainMaintenanceFeatures.swapchainMaintenance1 = vk::True;
			if (FeatureTail == &Vulkan13Features) Vulkan13Features.setPNext(&SwapchainMaintenanceFeatures);
			else if (FeatureTail == &Synchronization2Features) Synchronization2Features.setPNext(&SwapchainMaintenanceFeatures);
			else Vulkan11Features.setPNext(&SwapchainMaintenanceFeatures);
		}

		try
		{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Device);
#endif
			Device = Gpu.createDevice(DeviceInfo);
		}
		catch (const vk::SystemError& err)
		{
			throw std::runtime_error(std::format(
				"Vulkan logical-device creation failed: result={}, queueFamilies={}, extensions={}, error={}",
				vk::to_string(static_cast<vk::Result>(err.code().value())),
				1, DeviceExtensions.size(), err.what()));
		}
		catch (const std::runtime_error& err)
		{
			throw std::runtime_error(std::format(
				"Vulkan logical-device creation failed: result=unavailable, queueFamilies={}, extensions={}, error={}",
				1, DeviceExtensions.size(), err.what()));
		}

		GraphicsQueue = new FVulkanQueue(this, GraphicsQueueFamilyIndex);
		ComputeQueue = GraphicsQueue;
		TransferQueue = GraphicsQueue;
		PresentQueue = GraphicsQueue;
		DURIN_DEBUG("Vulkan queue selection: family={} shared by graphics, compute, transfer, and presentation.",
			GraphicsQueueFamilyIndex);
	}

	auto FVulkanDevice::SetupPresentQueue(vk::SurfaceKHR InSurface) -> bool
	{
		if (!Gpu.getSurfaceSupportKHR(GraphicsQueueFamilyIndex, InSurface))
		{
			DURIN_ERROR("Vulkan surface is incompatible with the provisioned graphics/presentation queue family {}.",
				GraphicsQueueFamilyIndex);
			return false;
		}
		return true;
	}

	auto FVulkanDevice::WaitUtilIdle() const -> void
	{
		CheckVulkanRHIThread();
		Device.waitIdle();
		if (CompletionTracker)
		{
			CompletionTracker->Poll();
		}
	}

	vk::Device FVulkanDevice::GetHandle() const
	{
		return Device;
	}

	vk::PhysicalDevice FVulkanDevice::GetGpu() const
	{
		return Gpu;
	}

	auto FVulkanDevice::GetRenderPassManager() const -> FVulkanRenderPassManager&
	{
		check(RenderPassManager);
		// ReSharper disable once CppDFANullDereference
		return *RenderPassManager;
	}

	auto FVulkanDevice::AcquireDeferredContext() -> FVulkanCommandListContext*
	{
		if (CommandContexts.empty())
		{
			return new FVulkanCommandListContext(GVulkanRHI, *this, GraphicsQueue);
		}
		FVulkanCommandListContext* Context = CommandContexts.back();

		return Context;
	}

	auto FVulkanDevice::ReleaseDeferredContext(FVulkanCommandListContext* Context) -> void
	{
		CommandContexts.push_back(Context);
	}

	auto FVulkanDevice::GetCurrentFrame() -> FVulkanFrame&
	{
		CheckVulkanRHIThread();
		return *Frames[CurrentFrameIndex];
	}

	auto FVulkanDevice::SetCurrentFrameIndex(uint32 FrameIndex) -> void
	{
		CheckVulkanRHIThread();
		check(FrameIndex < Frames.size());
		CurrentFrameIndex = FrameIndex;
	}

	auto FVulkanDevice::GetCurrentFrameIndex() const -> uint32
	{
		CheckVulkanRHIThread();
		return CurrentFrameIndex;
	}

	auto FVulkanDevice::NotifyDeleted_Image(vk::Image Image) -> void
	{
		GetRenderPassManager().NotifyDeleted_Image(Image);
	}

	auto FVulkanDevice::NotifyDeleted_GraphicsPipeline(
		FVulkanGraphicsPipelineState* PipelineState) -> void
	{
		CheckVulkanRHIThread();
		if (ImmediateContext)
		{
			ImmediateContext->NotifyDeleted_GraphicsPipeline(PipelineState);
		}
		for (FVulkanCommandListContext* Context : CommandContexts)
		{
			Context->NotifyDeleted_GraphicsPipeline(PipelineState);
		}
	}

	auto FVulkanDevice::Destroy() -> void
	{
		CheckVulkanRHIThread();
		if (!Device)
		{
			return;
		}
		WaitUtilIdle();
		if (CompletionTracker)
		{
			CompletionTracker->WaitForAll();
		}

		delete ImmediateContext;
		ImmediateContext = nullptr;

		for (FVulkanCommandListContext* Context : CommandContexts)
		{
			delete Context;
		}
		CommandContexts.clear();

		for (auto*& Frame : Frames)
		{
			delete Frame;
			Frame = nullptr;
		}
		delete ReadbackArena;
		ReadbackArena = nullptr;
		delete UploadArena;
		UploadArena = nullptr;
		delete GlobalDescriptorPool;
		GlobalDescriptorPool = nullptr;

		delete DynamicUniformBufferAllocator;
		DynamicUniformBufferAllocator = nullptr;
		delete DynamicStorageBufferAllocator;
		DynamicStorageBufferAllocator = nullptr;

		delete DescriptorSetCache;
		DescriptorSetCache = nullptr;

		delete PipelineManager;
		PipelineManager = nullptr;

		if (RenderPassManager)
		{
			RenderPassManager->ReleaseFramebuffers();
		}
		if (IsInRHIThread())
		{
			RHIFlushDeferredResources();
		}
		else
		{
			GCommandListExecutor.GetImmediateCommandList().ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources);
		}
		delete RenderPassManager;
		RenderPassManager = nullptr;
		DeferredDeletionQueue.Clear();
		MemoryManager.Deinit();

		delete GraphicsQueue;
		GraphicsQueue = nullptr;
		ComputeQueue = nullptr;
		TransferQueue = nullptr;
		PresentQueue = nullptr;

		delete CompletionTracker;
		CompletionTracker = nullptr;

		FenceManager.Deinit();

		DeferredDeletionQueue.ReleaseResources(true);

		Device.destroy();
		Device = nullptr;
	}
} // namespace Durin::VulkanRHI
