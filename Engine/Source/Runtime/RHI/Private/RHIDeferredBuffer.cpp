#include "Backend/RHIDeferredBufferBackend.h"
#include "RHICommandList.h"
#include "DynamicRHI.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> LivePayloadBytes = 0;
		std::atomic<uint64> PeakPayloadBytes = 0;
		std::atomic<uint64> BackingLiveBytes = 0, BackingPeakBytes = 0, BackingCapacity = 0;

		struct FPayloadAccountingGuard
		{
			uint64 Bytes = 0;
			~FPayloadAccountingGuard() { LivePayloadBytes.fetch_sub(Bytes, std::memory_order_relaxed); }
		};

		auto ValidateDeferredDescriptor(const FRHIBufferDesc& Desc,
			ERHIBufferLifetimeUsage Usage) -> bool
		{
			if (Usage != ERHIBufferLifetimeUsage::SingleDraw
				&& Usage != ERHIBufferLifetimeUsage::SingleFrame
				&& Usage != ERHIBufferLifetimeUsage::MultiFrame)
				return false;
			constexpr auto Allowed = EBufferUsageFlags::UniformBuffer
				| EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ByteAddressBuffer
				| EBufferUsageFlags::ShaderResource;
			if (EnumHasAnyFlags(Desc.Usage, ~Allowed))
				return false;
			const bool bUniform = EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer);
			const bool bStructured = EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::StructuredBuffer);
			const bool bByteAddress = EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::ByteAddressBuffer);
			if (static_cast<int>(bUniform) + bStructured + bByteAddress != 1)
				return false;
			if (Desc.Size == 0
				|| (bUniform && (Desc.Size % 16 != 0 || Desc.Stride != 0))
				|| (bStructured && (Desc.Stride == 0 || Desc.Size % Desc.Stride != 0))
				|| (bByteAddress && (Desc.Size % 4 != 0 || Desc.Stride != 4)))
				return false;
			return true;
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

	FRHIBufferUploadAccounting::~FRHIBufferUploadAccounting()
	{
		LivePayloadBytes.fetch_sub(Bytes, std::memory_order_relaxed);
	}

	auto FRHIBufferUploadAccounting::Track(uint64 Bytes)
		-> std::shared_ptr<const FRHIBufferUploadAccounting>
	{
		const uint64 Live = LivePayloadBytes.fetch_add(Bytes, std::memory_order_relaxed) + Bytes;
		FPayloadAccountingGuard Guard{Bytes};
		uint64 Peak = PeakPayloadBytes.load(std::memory_order_relaxed);
		while (Peak < Live && !PeakPayloadBytes.compare_exchange_weak(
			Peak, Live, std::memory_order_relaxed)) {}
		auto Result = std::unique_ptr<FRHIBufferUploadAccounting>(new FRHIBufferUploadAccounting(Bytes));
		Guard.Bytes = 0;
		return std::shared_ptr<const FRHIBufferUploadAccounting>(std::move(Result));
	}

	auto FRHIBufferUploadData::Copy(FByteView Data)
		-> std::shared_ptr<const FRHIBufferUploadData>
	{
		require(!Data.empty());
		auto Result = std::shared_ptr<FRHIBufferUploadData>(new FRHIBufferUploadData);
		Result->Accounting = FRHIBufferUploadAccounting::Track(Data.size());
		Result->Size = Data.size();
		Result->CopiedBytes = std::make_unique<std::byte[]>(Data.size());
		std::memcpy(Result->CopiedBytes.get(), Data.data(), Data.size());
		return Result;
	}

	auto FRHIBufferUploadData::Take(FByteBuffer Data)
		-> std::shared_ptr<const FRHIBufferUploadData>
	{
		require(!Data.empty());
		auto Result = std::shared_ptr<FRHIBufferUploadData>(new FRHIBufferUploadData);
		Result->Accounting = FRHIBufferUploadAccounting::Track(Data.capacity());
		Result->Owned = std::move(Data);
		return Result;
	}

	FRHIDeferredBufferSnapshot::FRHIDeferredBufferSnapshot(uint32 InSize, size_t InReferenceCount)
		: Data(std::make_unique<std::byte[]>(InSize)),
		References(InReferenceCount ? std::make_unique<TRefCountPtr<FRHIResource>[]>(InReferenceCount) : nullptr),
		Size(InSize), ReferenceCount(InReferenceCount) {}

	FRHIDeferredBufferSnapshot::~FRHIDeferredBufferSnapshot()
	{
		// Release owned allocations before updating the live-byte accounting.
		References.reset();
		Data.reset();
	}

	auto FRHIDeferredBufferSnapshot::Allocate(const FRHIBufferDesc& Desc,
		std::span<FRHIResource* const> References)
		-> std::shared_ptr<FRHIDeferredBufferSnapshot>
	{
		require(References.size() <= (UINT64_MAX - Desc.Size) / sizeof(TRefCountPtr<FRHIResource>));
		const uint64 Bytes = Desc.Size + References.size() * sizeof(TRefCountPtr<FRHIResource>);
		auto Accounting = FRHIBufferUploadAccounting::Track(Bytes);
		auto Admission = GDynamicRHI ? GDynamicRHI->RHIReserveBufferBacking(Desc) : std::shared_ptr<void>{};
		auto Result = std::shared_ptr<FRHIDeferredBufferSnapshot>(
			new FRHIDeferredBufferSnapshot(Desc.Size, References.size()));
		Result->BackingAdmission = std::move(Admission);
		Result->OwnedBytes = Bytes;
		Result->Accounting = std::move(Accounting);
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

	auto FRHICommandListBase::CreateStorageBuffer(const FRHIBufferDesc& Desc,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData)
		-> TRefCountPtr<FRHIBuffer>
	{
		require(IsRecording());
		requiref(ValidateDeferredDescriptor(Desc, Usage), "Invalid CPU-authored buffer descriptor or lifetime usage.");
		require(InitialData.size() == Desc.Size);
		require(!EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer));
		auto Snapshot = FRHIDeferredBufferSnapshot::Allocate(Desc, {});
		std::memcpy(Snapshot->Data.get(), InitialData.data(), Desc.Size);
		return TRefCountPtr<FRHIBuffer>(new FRHIBuffer(Desc, Usage, std::move(Snapshot)));
	}

	auto FRHICommandListBase::CreateUniformBufferRange(const void* Data, uint32 Size) -> FRHIUniformBufferRange
	{
		require(Data);
		auto Buffer = CreateUniformBuffer({Size}, ERHIBufferLifetimeUsage::SingleFrame,
			{static_cast<const std::byte*>(Data), Size});
		return {Buffer.GetReference(), 0, Size, Buffer.GetReference()};
	}

	auto FRHICommandListBase::CreateUniformBuffer(const FRHIUniformBufferLayout& Layout,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData, std::span<FRHIResource* const> References)
		-> TRefCountPtr<FRHIUniformBuffer>
	{
		require(IsRecording());
		const FRHIBufferDesc Desc{Layout.ConstantBufferSize, 0, EBufferUsageFlags::UniformBuffer};
		requiref(ValidateDeferredDescriptor(Desc, Usage), "Invalid CPU-authored buffer descriptor or lifetime usage.");
		require(InitialData.size() == Desc.Size);
		require(ValidateDeferredReferences(Desc, References));
		auto Snapshot = FRHIDeferredBufferSnapshot::Allocate(Desc, References);
		std::memcpy(Snapshot->Data.get(), InitialData.data(), Desc.Size);
		return TRefCountPtr<FRHIUniformBuffer>(new FRHIUniformBuffer(Layout, Usage, std::move(Snapshot)));
	}

	auto FRHICommandListBase::UpdateUniformBuffer(FRHIUniformBuffer* Buffer,
		FByteView Data, std::span<FRHIResource* const> References)
		-> void
	{
		UpdateCPUAuthoredBuffer(Buffer, 0, Data, References);
	}

	auto FRHICommandListBase::UpdateBuffer(FRHIBuffer* Buffer, uint32 Offset, FByteView Data)
		-> void
	{
		require(IsRecording());
		require(Buffer && !EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::UniformBuffer));
		UpdateCPUAuthoredBuffer(Buffer, Offset, Data, {});
	}

	auto FRHICommandListBase::UpdateCPUAuthoredBuffer(FRHIBuffer* Buffer,
		uint32 Offset, FByteView Data, std::span<FRHIResource* const> References)
		-> void
	{
		require(IsRecording());
		require(Buffer);
		require(Buffer->GetContentMode() == ERHIBufferContentMode::CPUAuthored);
		const auto& Desc = Buffer->GetDesc();
		require(!Data.empty() && Offset <= Desc.Size && Data.size() <= Desc.Size - Offset);
		require(!EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer)
			|| (Offset == 0 && Data.size() == Desc.Size));
		require(ValidateDeferredReferences(Desc, References));
		auto Snapshot = FRHIDeferredBufferSnapshot::Allocate(Desc, References);
		std::memcpy(Snapshot->Data.get() + Offset, Data.data(), Data.size());
		const auto OwnedBytes = Snapshot->GetOwnedPayloadBytes();
		EnqueueLambda([Buffer = TRefCountPtr<FRHIBuffer>(Buffer),
			Next = std::move(Snapshot), Offset, Size = static_cast<uint32>(Data.size())]() mutable {
			Buffer->ApplyUpdate(std::move(Next), Offset, Size);
		}, OwnedBytes);
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
