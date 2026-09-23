#pragma once

#include "RHIShaderParameters.h"
#include "Backend/RHIDeferredBufferBackend.h"

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
		FVulkanBindingAdmission(uint64 InAlignment, uint64 InCapacity, std::vector<FRHIQueueId> InQueues)
			: Alignment(InAlignment), MaxCapacity(InCapacity), Queues(std::move(InQueues)) {}
		~FVulkanBindingAdmission();
		auto Reserve(uint64 Size) -> std::shared_ptr<FReservation>;
	private:
		struct FPage { uint64 Size; FRHIQueueId Queue; std::map<uint64, uint64> Used; };
		auto Release(const std::vector<FSlot>& Slots) -> void;
		std::mutex Mutex;
		uint64 Alignment, MaxCapacity;
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

	// Keeps logical bindings separate from the physical descriptor snapshot.
	class FVulkanDeferredBufferBindings
	{
	public:
		auto Update(std::span<const FRHIShaderParameterResource> Parameters) -> void;
		auto Resolve(FVulkanDevice& Device, FVulkanCommandListContext& Context, ERHIPipeline Pipeline)
			-> std::vector<FRHIShaderParameterResource>;
		auto Clear() -> void { Bindings.clear(); }
	private:
		struct FResolved
		{
			std::shared_ptr<const FRHIDeferredBufferSnapshot> Snapshot;
			TRefCountPtr<FRHIBufferView> View;
			std::shared_ptr<void> Backing;
			std::shared_ptr<void> Lease;
		};
		struct FBinding
		{
			FRHIShaderParameterResource Parameter;
			TRefCountPtr<FRHIBufferView> Logical;
			std::weak_ptr<FResolved> Resolved;
		};
		std::vector<FBinding> Bindings;
	};
}
