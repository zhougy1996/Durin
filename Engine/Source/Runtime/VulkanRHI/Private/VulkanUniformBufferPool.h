#pragma once

#include "CoreMinimal.h"
#include "VulkanBuffer.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;

	// Front-end reservations contain no native resources and can outlive shutdown.
	class FVulkanUniformBufferAdmission : public std::enable_shared_from_this<FVulkanUniformBufferAdmission>
	{
	public:
		struct FSlot { uint32 Page; uint64 PageSize; uint64 Offset; uint64 Size; };
		struct FReservation
		{
			std::shared_ptr<FVulkanUniformBufferAdmission> Owner;
			FSlot Slot{};
			~FReservation();
		};
		explicit FVulkanUniformBufferAdmission(uint64 InAlignment) : Alignment(InAlignment) {}
		~FVulkanUniformBufferAdmission();
		auto Reserve(uint64 Size) -> std::shared_ptr<FReservation>;
	private:
		struct FPage { uint64 Size; std::map<uint64, uint64> Used; };
		auto Release(const FSlot& Slot) -> void;
		std::mutex Mutex;
		uint64 Alignment;
		uint64 Capacity = 0;
		std::vector<FPage> Pages;
	};

	// Native pages live exclusively on the RHI lane, separately from admission.
	class FVulkanUniformBufferPool
	{
	public:
		explicit FVulkanUniformBufferPool(FVulkanDevice& InDevice);
		~FVulkanUniformBufferPool();
		auto Reserve(uint64 Size) -> std::shared_ptr<FVulkanUniformBufferAdmission::FReservation> { return Admission->Reserve(Size); }
		auto Resolve(const std::shared_ptr<FVulkanUniformBufferAdmission::FReservation>& Reservation)
			-> std::pair<TRefCountPtr<FVulkanBuffer>, uint64>;
	private:
		FVulkanDevice& Device;
		std::shared_ptr<FVulkanUniformBufferAdmission> Admission;
		std::map<uint32, TRefCountPtr<FVulkanBuffer>> Buffers;
	};

}
