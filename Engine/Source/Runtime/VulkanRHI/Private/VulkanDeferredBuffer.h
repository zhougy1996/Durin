#pragma once

#include "CoreMinimal.h"

#include "RHI.h"

#include "RHIShaderParameters.h"
#include "Backend/RHIDeferredBufferBackend.h"
#include "VulkanUniformBuffer.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanBuffer;
	class FVulkanCommandListContext;

	// Front-end reservations contain no native resources and can outlive shutdown.
	class FVulkanBindingAdmission : public std::enable_shared_from_this<FVulkanBindingAdmission>
	{
	public:
		struct FSlot { uint32 Page; uint64 PageSize; FRHIQueueId Queue; uint64 Offset; uint64 Size; };
		struct FReservation
		{
			std::shared_ptr<FVulkanBindingAdmission> Owner;
			std::vector<FSlot> Slots;
			~FReservation();
		};
		FVulkanBindingAdmission(uint64 InAlignment, std::vector<FRHIQueueId> InQueues)
			: Alignment(InAlignment), Queues(std::move(InQueues)) {}
		~FVulkanBindingAdmission();
		auto Reserve(uint64 Size) -> std::shared_ptr<FReservation>;
	private:
		struct FPage { uint64 Size; FRHIQueueId Queue; std::map<uint64, uint64> Used; };
		auto Release(const std::vector<FSlot>& Slots) -> void;
		std::mutex Mutex;
		uint64 Alignment;
		uint64 Capacity = 0;
		std::vector<FRHIQueueId> Queues;
		std::vector<FPage> Pages;
	};

	// Native pages live exclusively on the RHI lane, separately from admission.
	class FVulkanBindingPool
	{
	public:
		FVulkanBindingPool(FVulkanDevice& InDevice, bool bInUniform);
		~FVulkanBindingPool();
		auto Reserve(uint64 Size) -> std::shared_ptr<void> { return Admission->Reserve(Size); }
		auto Resolve(const std::shared_ptr<void>& Reservation, FRHIQueueId Queue)
			-> std::pair<TRefCountPtr<FVulkanBuffer>, uint64>;
	private:
		FVulkanDevice& Device;
		bool bUniform;
		std::shared_ptr<FVulkanBindingAdmission> Admission;
		std::map<uint32, TRefCountPtr<FVulkanBuffer>> Buffers;
	};

	// Captures physical buffer bindings when shader parameters are bound. Updates require rebinding.
	class FVulkanShaderBufferBindings
	{
	public:
		auto Update(std::span<const FRHIShaderParameterResource> Parameters) -> void;
		auto Resolve(FVulkanDevice& Device, FVulkanCommandListContext& Context)
			-> std::vector<FRHIShaderParameterResource>;
		auto PrepareForUse(FVulkanCommandListContext& Context, ERHIPipeline Pipeline) -> void;
		auto Clear() -> void { Bindings.clear(); }
	private:
		struct FResolved
		{
			std::shared_ptr<const FRHIStorageBufferSnapshot> Snapshot;
			TRefCountPtr<FRHIBufferView> View;
			std::shared_ptr<void> Backing;
			std::shared_ptr<void> Lease;
			uint64 PhysicalOffset = 0;
			bool bDynamic = false;
		};
		struct FBinding
		{
			FRHIShaderParameterResource Parameter;
			TRefCountPtr<FRHIResource> Logical;
			std::shared_ptr<FResolved> Resolved;
			bool bDirty = true;
		};
		std::vector<FBinding> Bindings;
	};
}
