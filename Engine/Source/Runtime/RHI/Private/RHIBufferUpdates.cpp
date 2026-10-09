#include "Backend/RHIStorageBuffer.h"
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
				if (IsLogicalBufferBindingResource(Reference)) return false;
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

	auto FRHIBufferUploadAccounting::RecordBackingAdmission(uint64 Bytes, uint64 Capacity, bool bAcquire) -> void
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

	FRHIStorageBuffer::FRHIStorageBuffer(const FRHIBufferDesc& InDesc,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData)
		: FRHIBuffer(InDesc, ERHIResourceType::StorageBuffer), LifetimeUsage(Usage),
		Accounting(FRHIBufferUploadAccounting::Track(InDesc.Size)),
		Contents(InitialData.begin(), InitialData.end())
	{
		require(InitialData.size() == InDesc.Size);
	}

	auto FRHIStorageBuffer::UpdateContents(uint32 Offset, FByteView Data) -> void
	{
		require(IsExecutingRHICommands());
		require(!Data.empty() && Offset <= GetSize() && Data.size() <= GetSize() - Offset);
		std::memcpy(Contents.data() + Offset, Data.data(), Data.size());
	}

	auto FRHICommandListBase::CreateStorageBuffer(const FRHIBufferDesc& Desc,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData)
		-> TRefCountPtr<FRHIBuffer>
	{
		require(IsRecording());
		requiref(ValidateDeferredDescriptor(Desc, Usage), "Invalid upload buffer descriptor or lifetime usage.");
		require(InitialData.size() == Desc.Size);
		require(!EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer));
		return GDynamicRHI ? GDynamicRHI->RHICreateStorageBuffer(Desc, Usage, InitialData)
			: TRefCountPtr<FRHIBuffer>(new FRHIStorageBuffer(Desc, Usage, InitialData));
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
		requiref(ValidateDeferredDescriptor(Desc, Usage), "Invalid upload buffer descriptor or lifetime usage.");
		require(InitialData.size() == Desc.Size);
		require(ValidateDeferredReferences(Desc, References));
		return GDynamicRHI ? GDynamicRHI->RHICreateUniformBuffer(Layout, Usage, InitialData, References)
			: TRefCountPtr<FRHIUniformBuffer>(new FRHIUniformBuffer(Layout, Usage, InitialData, References));
	}

	FRHIUniformBuffer::FRHIUniformBuffer(const FRHIUniformBufferLayout& InLayout,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData, std::span<FRHIResource* const> References)
		: FRHIResource(ERHIResourceType::UniformBuffer), Layout(InLayout), LifetimeUsage(Usage),
		PendingData(FRHIBufferUploadData::Copy(InitialData))
	{
		for (auto* Reference : References) PendingReferences.emplace_back(Reference);
	}

	auto FRHIUniformBuffer::UpdateContents(FByteView Data, std::span<FRHIResource* const> References) -> void
	{
		require(IsExecutingRHICommands());
		PendingData = FRHIBufferUploadData::Copy(Data);
		PendingReferences.clear();
		for (auto* Reference : References) PendingReferences.emplace_back(Reference);
	}

	auto FDynamicRHI::RHICreateStorageBuffer(const FRHIBufferDesc& Desc,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData) -> TRefCountPtr<FRHIBuffer>
	{
		return new FRHIStorageBuffer(Desc, Usage, InitialData);
	}

	auto FDynamicRHI::RHICreateUniformBuffer(const FRHIUniformBufferLayout& Layout,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData, std::span<FRHIResource* const> References)
		-> TRefCountPtr<FRHIUniformBuffer>
	{
		return new FRHIUniformBuffer(Layout, Usage, InitialData, References);
	}

	auto FRHICommandListBase::UpdateUniformBuffer(FRHIUniformBuffer* Buffer,
		FByteView Data, std::span<FRHIResource* const> References)
		-> void
	{
		require(IsRecording() && Buffer && Data.size() == Buffer->GetSize());
		require(ValidateDeferredReferences({Buffer->GetSize(), 0, EBufferUsageFlags::UniformBuffer}, References));
		auto Upload = FRHIBufferUploadData::Copy(Data);
		std::vector<TRefCountPtr<FRHIResource>> Owners;
		for (auto* Reference : References) Owners.emplace_back(Reference);
		const auto Bytes = Data.size() + Owners.size() * sizeof(TRefCountPtr<FRHIResource>);
		EnqueueLambda([Buffer = TRefCountPtr<FRHIUniformBuffer>(Buffer),
			Upload = std::move(Upload), Owners = std::move(Owners)]() mutable {
			std::vector<FRHIResource*> References;
			for (const auto& Owner : Owners) References.push_back(Owner.GetReference());
			Buffer->UpdateContents(Upload->GetData(), References);
			Upload.reset();
			Owners.clear();
			Buffer = nullptr;
		}, Bytes);
	}

	auto FRHICommandListBase::UpdateBuffer(FRHIBuffer* Buffer, uint32 Offset, FByteView Data)
		-> void
	{
		require(IsRecording() && IsStorageBuffer(Buffer));
		const auto& Desc = Buffer->GetDesc();
		require(!Data.empty() && Offset <= Desc.Size && Data.size() <= Desc.Size - Offset);
		require(!EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::UniformBuffer));
		auto Upload = FRHIBufferUploadData::Copy(Data);
		const auto OwnedBytes = Upload->GetOwnedPayloadBytes();
		EnqueueLambda([Buffer = TRefCountPtr<FRHIStorageBuffer>(static_cast<FRHIStorageBuffer*>(Buffer)),
			Upload = std::move(Upload), Offset]() mutable {
			Buffer->UpdateContents(Offset, Upload->GetData());
			Upload.reset();
			Buffer = nullptr;
		}, OwnedBytes);
	}

	auto FRHIBufferView::CanCreate(FRHIBuffer* Buffer, const FRHIBufferViewDesc& Desc) -> bool
	{
		if (!IsStorageBuffer(Buffer) || !ValidateBufferViewDesc(Buffer->GetDesc(), Desc)) return false;
		if (const auto* Caps = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr)
		{
			const bool bUniform = Desc.Type == ERHIBufferViewType::Uniform;
			const uint32 Alignment = bUniform ? Caps->MinUniformBufferOffsetAlignment : Caps->MinStorageBufferOffsetAlignment;
			const uint32 Limit = bUniform ? Caps->MaxUniformBufferRange : Caps->MaxStorageBufferRange;
			if ((Alignment && Desc.Offset % Alignment != 0) || (Limit && Desc.Size > Limit)) return false;
		}
		return true;
	}

	auto FRHIBufferView::Create(FRHIBuffer* Buffer, const FRHIBufferViewDesc& Desc)
		-> TRefCountPtr<FRHIBufferView>
	{
		requiref(CanCreate(Buffer, Desc), "Invalid Storage buffer view description or parent.");
		return TRefCountPtr<FRHIBufferView>(new FRHIBufferView(Buffer, Desc));
	}
}
