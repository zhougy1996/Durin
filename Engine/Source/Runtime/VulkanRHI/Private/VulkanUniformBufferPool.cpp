#include "VulkanUniformBufferPool.h"
#include "RHIBufferUploadData.h"
#include "VulkanDevice.h"
#include "VulkanDiagnostics.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanUniformBufferAdmission::FReservation::~FReservation()
	{
		if (Owner && Slot.Size) Owner->Release(Slot);
	}

	auto FVulkanUniformBufferAdmission::Release(const FSlot& Slot) -> void
	{
		std::lock_guard Lock(Mutex);
		Pages[Slot.Page].Used.erase(Slot.Offset);
		FRHIBufferUploadAccounting::RecordBackingAdmission(Slot.Size, 0, false);
	}

	FVulkanUniformBufferAdmission::~FVulkanUniformBufferAdmission()
	{
		FRHIBufferUploadAccounting::RecordBackingAdmission(0, Capacity, false);
	}

	auto FVulkanUniformBufferAdmission::Reserve(uint64 Size) -> std::shared_ptr<FReservation>
	{
		require(Size != 0 && Size <= UINT32_MAX - (Alignment - 1));
		Size = (Size + Alignment - 1) / Alignment * Alignment;
		auto Result = std::make_shared<FReservation>();
		Result->Owner = shared_from_this();
		std::lock_guard Lock(Mutex);
		for (uint32 Index = 0; Index < Pages.size(); ++Index)
		{
			auto& Page = Pages[Index];
			if (Page.Size < Size) continue;
			uint64 Offset = 0;
			for (const auto& [Start, Length] : Page.Used)
			{
				if (Size <= Start - Offset) break;
				Offset = Start + Length;
			}
			if (Size > Page.Size - Offset) continue;
			Page.Used.emplace(Offset, Size);
			Result->Slot = {Index, Page.Size, Offset, Size};
			FRHIBufferUploadAccounting::RecordBackingAdmission(Size, 0, true);
			return Result;
		}
		const uint64 PageSize = std::max<uint64>(4ull * 1024 * 1024, Size);
		Pages.push_back({PageSize, {{0, Size}}});
		Capacity += PageSize;
		Result->Slot = {static_cast<uint32>(Pages.size() - 1), PageSize, 0, Size};
		FRHIBufferUploadAccounting::RecordBackingAdmission(Size, PageSize, true);
		return Result;
	}

	auto TestVulkanUniformBufferAdmission() -> bool
	{
		constexpr uint64 PageSize = 4ull * 1024 * 1024;
		auto Admission = std::make_shared<FVulkanUniformBufferAdmission>(256);
		auto A = Admission->Reserve(1), B = Admission->Reserve(1), Tail = Admission->Reserve(PageSize - 512);
		if (!A || !B || !Tail) return false;
		if (A->Slot.Offset != 0 || B->Slot.Offset != 256
			|| Tail->Slot.Offset != 512) return false;
		// Fragmented pages grow without waiting; released adjacent intervals are reused.
		B.reset();
		auto Grown = Admission->Reserve(512);
		if (!Grown || Grown->Slot.Page == A->Slot.Page) return false;
		A.reset();
		auto Reused = Admission->Reserve(512);
		if (!Reused || Reused->Slot.Offset != 0) return false;
		Reused.reset(); Tail.reset();
		std::vector<std::shared_ptr<FVulkanUniformBufferAdmission::FReservation>> Concurrent(16);
		std::vector<std::thread> Threads;
		for (size_t Index = 0; Index < Concurrent.size(); ++Index)
			Threads.emplace_back([&, Index] { Concurrent[Index] = Admission->Reserve(PageSize / 8); });
		for (auto& Thread : Threads) Thread.join();
		return std::ranges::count_if(Concurrent, [](const auto& Item) { return bool(Item); }) == 16;
	}

	FVulkanUniformBufferPool::FVulkanUniformBufferPool(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		const auto& Limits = Device.GetGpuProperties().limits;
		Admission = std::make_shared<FVulkanUniformBufferAdmission>(std::max<uint64>({16, Limits.nonCoherentAtomSize,
			Limits.minUniformBufferOffsetAlignment}));
	}

	FVulkanUniformBufferPool::~FVulkanUniformBufferPool()
	{
		CheckVulkanRHIThread();
		for (const auto& [Index, Buffer] : Buffers)
			if (Buffer) GVulkanMemoryBaselineTracker.RecordArenaPageFreed(EVulkanAllocationClassCandidate::DynamicUpload, Buffer->GetSize());
	}

	auto FVulkanUniformBufferPool::Resolve(const std::shared_ptr<FVulkanUniformBufferAdmission::FReservation>& Reservation)
		-> std::pair<TRefCountPtr<FVulkanBuffer>, uint64>
	{
		CheckVulkanRHIThread();
		require(Reservation && Reservation->Owner == Admission && Reservation->Slot.Size);
		const auto& Slot = Reservation->Slot;
		auto& Buffer = Buffers[Slot.Page];
		if (!Buffer)
		{
			const auto Usage = EBufferUsageFlags::Dynamic | EBufferUsageFlags::UniformBuffer;
			Buffer = new FVulkanBuffer(Device, FRHIBufferCreateDesc::Create("UniformBindingPage",
				static_cast<uint32>(Slot.PageSize), 0, Usage), true);
			GVulkanMemoryBaselineTracker.RecordArenaPageAllocated(EVulkanAllocationClassCandidate::DynamicUpload, Slot.PageSize);
		}
		return {Buffer, Slot.Offset};
	}

	auto ReserveVulkanUniformBufferBackingForTest(uint32 Size) -> std::shared_ptr<void>
	{
		auto* Device = FVulkanDynamicRHI::Get().GetDeviceForTesting();
		require(Device);
		return Device->GetUniformBufferPool().Reserve(Size);
	}
}
