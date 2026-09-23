#include "Backend/RHIDeferredBufferBackend.h"
#include "RHICommandList.h"
#include "DynamicRHI.h"

namespace Durin
{
	namespace
	{
		constexpr uint64 MaxSingleUploadBytes = FRHIBufferUploadReservation::MaxSingleBytes;
		constexpr uint64 MaxPayloadBytes = FRHIBufferUploadReservation::MaxLiveBytes;
		std::atomic<uint64> LivePayloadBytes = 0;
		std::atomic<uint64> PeakPayloadBytes = 0;
		std::atomic<uint64> RejectedPayloadCount = 0;
		std::atomic<uint64> BackingLiveBytes = 0, BackingPeakBytes = 0, BackingCapacity = 0;

		struct FPayloadReservation
		{
			uint64 Bytes = 0;
			~FPayloadReservation() { LivePayloadBytes.fetch_sub(Bytes, std::memory_order_relaxed); }
		};

		auto ValidateDeferredDescriptor(const FRHIBufferDesc& Desc,
			ERHIBufferLifetimeUsage Usage) -> std::expected<void, ERHIBufferUploadError>
		{
			if (Usage != ERHIBufferLifetimeUsage::SingleDraw
				&& Usage != ERHIBufferLifetimeUsage::SingleFrame
				&& Usage != ERHIBufferLifetimeUsage::MultiFrame)
				return std::unexpected(ERHIBufferUploadError::InvalidUsage);
			constexpr auto Allowed = EBufferUsageFlags::UniformBuffer
				| EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ByteAddressBuffer
				| EBufferUsageFlags::ShaderResource;
			if (EnumHasAnyFlags(Desc.Usage, ~Allowed))
				return std::unexpected(ERHIBufferUploadError::InvalidUsage);
			const bool bUniform = EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer);
			const bool bStructured = EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::StructuredBuffer);
			const bool bByteAddress = EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::ByteAddressBuffer);
			if (static_cast<int>(bUniform) + bStructured + bByteAddress != 1)
				return std::unexpected(ERHIBufferUploadError::InvalidUsage);
			if (Desc.Size == 0
				|| (bUniform && (Desc.Size % 16 != 0 || Desc.Stride != 0))
				|| (bStructured && (Desc.Stride == 0 || Desc.Size % Desc.Stride != 0))
				|| (bByteAddress && (Desc.Size % 4 != 0 || Desc.Stride != 4)))
				return std::unexpected(ERHIBufferUploadError::InvalidDescriptor);
			return {};
		}

		auto ValidateDeferredReferences(const FRHIBufferDesc& Desc,
			std::span<FRHIResource* const> References) -> bool
		{
			if (!References.empty() && !EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer))
				return false;
			for (const auto* Reference : References)
			{
				// Sidecars retain physical resources, not a potentially cyclic logical graph.
				if (Reference && ((Reference->GetResourceType() == ERHIResourceType::Buffer
					&& static_cast<const FRHIBuffer*>(Reference)->GetContentMode() == ERHIBufferContentMode::CPUAuthored)
					|| (Reference->GetResourceType() == ERHIResourceType::BufferView
					&& static_cast<const FRHIBufferView*>(Reference)->GetBuffer()->GetContentMode() == ERHIBufferContentMode::CPUAuthored))) return false;
			}
			return true;
		}
	}

	auto GetBufferUploadStats() -> FRHIBufferUploadStats
	{
		return {LivePayloadBytes.load(std::memory_order_relaxed),
			PeakPayloadBytes.load(std::memory_order_relaxed),
			RejectedPayloadCount.load(std::memory_order_relaxed),
			BackingLiveBytes.load(), BackingPeakBytes.load(), BackingCapacity.load()};
	}

	auto FRHIDeferredBufferBackend::RecordAdmission(uint64 Bytes, uint64 Capacity, bool bAcquire) -> void
	{
		if (!bAcquire)
		{
			BackingLiveBytes.fetch_sub(Bytes);
			BackingCapacity.fetch_sub(Capacity);
			return;
		}
		BackingCapacity.fetch_add(Capacity);
		const auto Live = BackingLiveBytes.fetch_add(Bytes) + Bytes;
		auto Peak = BackingPeakBytes.load();
		while (Peak < Live && !BackingPeakBytes.compare_exchange_weak(Peak, Live)) {}
	}

	FRHIBufferUploadReservation::~FRHIBufferUploadReservation()
	{
		LivePayloadBytes.fetch_sub(Bytes, std::memory_order_relaxed);
	}

	auto FRHIBufferUploadReservation::TryReserve(uint64 Bytes)
		-> std::expected<std::shared_ptr<const FRHIBufferUploadReservation>, ERHIBufferUploadError>
	{
		uint64 Live = LivePayloadBytes.load(std::memory_order_relaxed);
		do
		{
			if (Bytes > MaxLiveBytes - Live)
			{
				RejectedPayloadCount.fetch_add(1, std::memory_order_relaxed);
				return std::unexpected(ERHIBufferUploadError::PayloadBudgetExceeded);
			}
		} while (!LivePayloadBytes.compare_exchange_weak(Live, Live + Bytes, std::memory_order_relaxed));
		FPayloadReservation Guard{Bytes};
		uint64 Peak = PeakPayloadBytes.load(std::memory_order_relaxed);
		while (Peak < Live + Bytes && !PeakPayloadBytes.compare_exchange_weak(
			Peak, Live + Bytes, std::memory_order_relaxed)) {}
		auto Result = std::unique_ptr<FRHIBufferUploadReservation>(new FRHIBufferUploadReservation(Bytes));
		Guard.Bytes = 0;
		return std::shared_ptr<const FRHIBufferUploadReservation>(std::move(Result));
	}

	auto FRHIBufferUploadData::TryCopy(FByteView Data)
		-> std::expected<std::shared_ptr<const FRHIBufferUploadData>, ERHIBufferUploadError>
	{
		if (Data.empty()) return std::unexpected(ERHIBufferUploadError::InvalidRange);
		if (Data.size() > FRHIBufferUploadReservation::MaxSingleBytes)
		{
			RejectedPayloadCount.fetch_add(1, std::memory_order_relaxed);
			return std::unexpected(ERHIBufferUploadError::PayloadBudgetExceeded);
		}
		auto Reservation = FRHIBufferUploadReservation::TryReserve(Data.size());
		if (!Reservation) return std::unexpected(Reservation.error());
		auto Result = std::shared_ptr<FRHIBufferUploadData>(new FRHIBufferUploadData);
		Result->Reservation = std::move(*Reservation);
		Result->Size = Data.size();
		Result->Copy = std::make_unique<std::byte[]>(Data.size());
		std::memcpy(Result->Copy.get(), Data.data(), Data.size());
		return Result;
	}

	auto FRHIBufferUploadData::TryTake(FByteBuffer Data)
		-> std::expected<std::shared_ptr<const FRHIBufferUploadData>, ERHIBufferUploadError>
	{
		if (Data.empty()) return std::unexpected(ERHIBufferUploadError::InvalidRange);
		if (Data.capacity() > FRHIBufferUploadReservation::MaxSingleBytes)
		{
			RejectedPayloadCount.fetch_add(1, std::memory_order_relaxed);
			return std::unexpected(ERHIBufferUploadError::PayloadBudgetExceeded);
		}
		auto Reservation = FRHIBufferUploadReservation::TryReserve(Data.capacity());
		if (!Reservation) return std::unexpected(Reservation.error());
		auto Result = std::shared_ptr<FRHIBufferUploadData>(new FRHIBufferUploadData);
		Result->Reservation = std::move(*Reservation);
		Result->Owned = std::move(Data);
		return Result;
	}

	FRHIDeferredBufferSnapshot::FRHIDeferredBufferSnapshot(uint32 InSize, size_t InReferenceCount)
		: Data(std::make_unique<std::byte[]>(InSize)),
		References(InReferenceCount ? std::make_unique<TRefCountPtr<FRHIResource>[]>(InReferenceCount) : nullptr),
		Size(InSize), ReferenceCount(InReferenceCount) {}

	FRHIDeferredBufferSnapshot::~FRHIDeferredBufferSnapshot()
	{
		// Release owned allocations before making their budget available again.
		References.reset();
		Data.reset();
	}

	auto FRHIDeferredBufferSnapshot::TryAllocate(const FRHIBufferDesc& Desc,
		std::span<FRHIResource* const> References)
		-> std::expected<std::shared_ptr<FRHIDeferredBufferSnapshot>, ERHIBufferUploadError>
	{
		const auto Size = Desc.Size;
		if (Size > MaxSingleUploadBytes
			|| References.size() > (MaxPayloadBytes - Size) / sizeof(TRefCountPtr<FRHIResource>))
		{
			RejectedPayloadCount.fetch_add(1, std::memory_order_relaxed);
			return std::unexpected(ERHIBufferUploadError::PayloadBudgetExceeded);
		}
		const uint64 Bytes = Size + References.size() * sizeof(TRefCountPtr<FRHIResource>);
		auto Reservation = FRHIBufferUploadReservation::TryReserve(Bytes);
		if (!Reservation) return std::unexpected(Reservation.error());
		auto Admission = GDynamicRHI ? GDynamicRHI->RHIReserveBufferBacking(Desc)
			: std::expected<std::shared_ptr<void>, ERHIBufferUploadError>{std::shared_ptr<void>{}};
		if (!Admission) return std::unexpected(Admission.error());
		auto Result = std::shared_ptr<FRHIDeferredBufferSnapshot>(
			new FRHIDeferredBufferSnapshot(Size, References.size()));
		Result->BackingAdmission = std::move(*Admission);
		Result->OwnedBytes = Bytes;
		Result->Reservation = std::move(*Reservation);
		for (size_t Index = 0; Index < References.size(); ++Index)
			Result->References[Index] = References[Index];
		return Result;
	}

	FRHIBuffer::FRHIBuffer(const FRHIBufferDesc& InDesc,
		ERHIBufferLifetimeUsage InUsage, std::shared_ptr<const FRHIDeferredBufferSnapshot> Initial)
		: FRHIResource(ERHIResourceType::Buffer), Desc(InDesc),
		ContentMode(ERHIBufferContentMode::CPUAuthored), LifetimeUsage(InUsage),
		Current(std::move(Initial)) {}


	auto FRHIBuffer::ApplyUpdate(std::shared_ptr<FRHIDeferredBufferSnapshot> Next,
		uint32 Offset, uint32 Size) -> void
	{
		require(IsExecutingRHICommands());
		// Preserve bytes from the replay-visible predecessor, never recording order.
		if (Offset != 0) std::memcpy(Next->Data.get(), Current->Data.get(), Offset);
		const uint32 End = Offset + Size;
		if (End < Desc.Size)
			std::memcpy(Next->Data.get() + End, Current->Data.get() + End, Desc.Size - End);
		require(Current->Version != UINT64_MAX);
		Next->Version = Current->Version + 1;
		Current = std::move(Next);
	}

	auto FRHIDeferredBufferBackend::ResolveSnapshot(const FRHIBuffer& Buffer)
		-> std::shared_ptr<const FRHIDeferredBufferSnapshot>
	{
		require(IsExecutingRHICommands() && Buffer.GetContentMode() == ERHIBufferContentMode::CPUAuthored);
		return Buffer.Current;
	}

	auto FRHIDeferredBufferBackend::GetBacking(const FRHIDeferredBufferSnapshot& Snapshot,
		const void* Context) -> std::shared_ptr<void>
	{
		require(IsExecutingRHICommands());
		const auto It = Snapshot.Backings.find(Context);
		return It == Snapshot.Backings.end() ? nullptr : It->second.lock();
	}

	auto FRHIDeferredBufferBackend::SetBacking(const FRHIDeferredBufferSnapshot& Snapshot,
		const void* Context, std::shared_ptr<void> Backing) -> void
	{
		require(IsExecutingRHICommands() && Context && Backing);
		require(Snapshot.Backings[Context].expired());
		Snapshot.Backings[Context] = std::move(Backing);
	}

	auto FRHICommandListBase::TryCreateStorageBuffer(const FRHIBufferDesc& Desc,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData)
		-> std::expected<TRefCountPtr<FRHIBuffer>, ERHIBufferUploadError>
	{
		require(IsRecording());
		if (const auto Valid = ValidateDeferredDescriptor(Desc, Usage); !Valid)
			return std::unexpected(Valid.error());
		if (InitialData.size() != Desc.Size)
			return std::unexpected(ERHIBufferUploadError::InvalidRange);
		if (EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer))
			return std::unexpected(ERHIBufferUploadError::InvalidUsage);
		auto Snapshot = FRHIDeferredBufferSnapshot::TryAllocate(Desc, {});
		if (!Snapshot) return std::unexpected(Snapshot.error());
		std::memcpy((*Snapshot)->Data.get(), InitialData.data(), Desc.Size);
		return TRefCountPtr<FRHIBuffer>(new FRHIBuffer(Desc, Usage, std::move(*Snapshot)));
	}

	auto FRHICommandListBase::CreateUniformBufferRange(const void* Data, uint32 Size) -> FRHIUniformBufferRange
	{
		if (!Data) throw std::runtime_error("Uniform upload has no source data.");
		auto Buffer = TryCreateUniformBuffer({Size}, ERHIBufferLifetimeUsage::SingleFrame,
			{static_cast<const std::byte*>(Data), Size});
		if (!Buffer) throw std::runtime_error("Uniform upload admission rejected.");
		return {Buffer->GetReference(), 0, Size, Buffer->GetReference()};
	}

	auto FRHICommandListBase::TryCreateUniformBuffer(const FRHIUniformBufferLayout& Layout,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData, std::span<FRHIResource* const> References)
		-> std::expected<TRefCountPtr<FRHIUniformBuffer>, ERHIBufferUploadError>
	{
		require(IsRecording());
		const FRHIBufferDesc Desc{Layout.ConstantBufferSize, 0, EBufferUsageFlags::UniformBuffer};
		if (const auto Valid = ValidateDeferredDescriptor(Desc, Usage); !Valid)
			return std::unexpected(Valid.error());
		if (InitialData.size() != Desc.Size)
			return std::unexpected(ERHIBufferUploadError::InvalidRange);
		if (!ValidateDeferredReferences(Desc, References))
			return std::unexpected(ERHIBufferUploadError::InvalidUsage);
		auto Snapshot = FRHIDeferredBufferSnapshot::TryAllocate(Desc, References);
		if (!Snapshot) return std::unexpected(Snapshot.error());
		std::memcpy((*Snapshot)->Data.get(), InitialData.data(), Desc.Size);
		return TRefCountPtr<FRHIUniformBuffer>(new FRHIUniformBuffer(Layout, Usage, std::move(*Snapshot)));
	}

	auto FRHICommandListBase::TryUpdateUniformBuffer(FRHIUniformBuffer* Buffer,
		FByteView Data, std::span<FRHIResource* const> References)
		-> std::expected<void, ERHIBufferUploadError>
	{
		return TryUpdateCPUAuthoredBuffer(Buffer, 0, Data, References);
	}

	auto FRHICommandListBase::TryUpdateBuffer(FRHIBuffer* Buffer, uint32 Offset, FByteView Data)
		-> std::expected<void, ERHIBufferUploadError>
	{
		require(IsRecording());
		if (Buffer && EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::UniformBuffer))
			return std::unexpected(ERHIBufferUploadError::InvalidUsage);
		return TryUpdateCPUAuthoredBuffer(Buffer, Offset, Data, {});
	}

	auto FRHICommandListBase::TryUpdateCPUAuthoredBuffer(FRHIBuffer* Buffer,
		uint32 Offset, FByteView Data, std::span<FRHIResource* const> References)
		-> std::expected<void, ERHIBufferUploadError>
	{
		require(IsRecording());
		if (!Buffer) return std::unexpected(ERHIBufferUploadError::InvalidDescriptor);
		if (Buffer->GetContentMode() != ERHIBufferContentMode::CPUAuthored)
			return std::unexpected(ERHIBufferUploadError::InvalidUsage);
		const auto& Desc = Buffer->GetDesc();
		if (Data.empty() || Offset > Desc.Size || Data.size() > Desc.Size - Offset
			|| (EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer)
				&& (Offset != 0 || Data.size() != Desc.Size)))
			return std::unexpected(ERHIBufferUploadError::InvalidRange);
		if (!ValidateDeferredReferences(Desc, References))
			return std::unexpected(ERHIBufferUploadError::InvalidUsage);
		auto Snapshot = FRHIDeferredBufferSnapshot::TryAllocate(Desc, References);
		if (!Snapshot) return std::unexpected(Snapshot.error());
		std::memcpy((*Snapshot)->Data.get() + Offset, Data.data(), Data.size());
		const auto OwnedBytes = (*Snapshot)->GetOwnedPayloadBytes();
		EnqueueLambda([Buffer = TRefCountPtr<FRHIBuffer>(Buffer),
			Next = std::move(*Snapshot), Offset, Size = static_cast<uint32>(Data.size())]() mutable {
			Buffer->ApplyUpdate(std::move(Next), Offset, Size);
		}, OwnedBytes);
		return {};
	}

	auto FRHICommandListBase::TryCreateBufferView(FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc)
		-> std::expected<TRefCountPtr<FRHIBufferView>, ERHIBufferUploadError>
	{
		require(IsRecording());
		return FRHIBufferView::TryCreate(Buffer, Desc);
	}

	auto FRHIBufferView::TryCreate(FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc)
		-> std::expected<TRefCountPtr<FRHIBufferView>, ERHIBufferUploadError>
	{
		if (!Buffer) return std::unexpected(ERHIBufferUploadError::InvalidDescriptor);
		if (Buffer->GetContentMode() != ERHIBufferContentMode::CPUAuthored)
			return std::unexpected(ERHIBufferUploadError::InvalidUsage);
		const auto Valid = ValidateBufferViewDesc(Buffer->GetDesc(), Desc);
		if (!Valid)
		{
			const auto Error = Valid.error();
			return std::unexpected(Error == ERHIBufferViewError::EmptyRange
				|| Error == ERHIBufferViewError::RangeOutOfBounds
				? ERHIBufferUploadError::InvalidRange : ERHIBufferUploadError::InvalidDescriptor);
		}
		if (const auto* Caps = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr)
		{
			const bool bUniform = Desc.Type == ERHIBufferViewType::Uniform;
			const uint32 Alignment = bUniform ? Caps->MinUniformBufferOffsetAlignment : Caps->MinStorageBufferOffsetAlignment;
			const uint32 Limit = bUniform ? Caps->MaxUniformBufferRange : Caps->MaxStorageBufferRange;
			if ((Alignment && Desc.Offset % Alignment != 0) || (Limit && Desc.Size > Limit))
				return std::unexpected(ERHIBufferUploadError::InvalidRange);
		}
		return TRefCountPtr<FRHIBufferView>(new FRHIBufferView(Buffer, Desc));
	}
}
