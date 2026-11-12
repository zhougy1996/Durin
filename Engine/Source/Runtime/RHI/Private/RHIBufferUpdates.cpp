#include "RHIBufferUploadData.h"
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

		auto ValidateUniformBufferLayout(const FRHIUniformBufferLayout& Layout,
			ERHIBufferLifetimeUsage Usage) -> bool
		{
			if (Usage != ERHIBufferLifetimeUsage::SingleDraw
				&& Usage != ERHIBufferLifetimeUsage::SingleFrame
				&& Usage != ERHIBufferLifetimeUsage::MultiFrame)
				return false;
			if (Layout.ConstantBufferSize == 0 || Layout.ConstantBufferSize % 16 != 0) return false;
			return true;
		}

		auto ValidateUniformBufferReferences(const FRHIUniformBufferLayout& Layout,
			std::span<FRHIResource* const> References, const FRHIUniformBuffer* Destination = nullptr) -> bool
		{
			if (!Layout.Resources.empty() && Layout.Resources.size() != References.size()) return false;
			for (size_t Index = 0; Index < References.size(); ++Index)
			{
				const auto* Reference = References[Index];
				if (!Reference) continue;
				if (IsUniformBufferResource(Reference))
				{
					if (Layout.Resources.empty() || (Layout.Resources[Index].Type != ERHIBindingType::UniformBuffer
						&& Layout.Resources[Index].Type != ERHIBindingType::UniformBufferDynamic)) return false;
					// Validate the actual ownership chain only on updates; no dependency registry.
					std::vector<const FRHIUniformBuffer*> Pending{static_cast<const FRHIUniformBuffer*>(Reference)};
					std::unordered_set<const FRHIUniformBuffer*> Visited;
					while (!Pending.empty())
					{
						const auto* Uniform = Pending.back(); Pending.pop_back();
						if (Uniform == Destination) return false;
						if (!Visited.insert(Uniform).second) continue;
						for (const auto& Child : Uniform->GetResourceTable())
							if (IsUniformBufferResource(Child.GetReference())) Pending.push_back(static_cast<const FRHIUniformBuffer*>(Child.GetReference()));
					}
				}
				if (Layout.Resources.empty()) continue;
				const auto& Member = Layout.Resources[Index];
				const auto Type = Reference->GetResourceType();
				if (Member.Type == ERHIBindingType::Sampler && Type != ERHIResourceType::Sampler) return false;
				if (Member.Type == ERHIBindingType::Texture && Type != ERHIResourceType::Texture
					&& Type != ERHIResourceType::TextureReference && Type != ERHIResourceType::TextureView) return false;
				if (Member.FallbackResourceIndex != INDEX_NONE_U32)
				{
					const auto Fallback = Member.FallbackResourceIndex;
					if (Member.Type != ERHIBindingType::Texture || Fallback >= References.size()
						|| !References[Fallback] || References[Fallback]->GetResourceType() != ERHIResourceType::Texture) return false;
				}
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
		requiref(ValidateUniformBufferLayout(Layout, Usage), "Invalid uniform buffer layout or lifetime usage.");
		require(InitialData.size() == Layout.ConstantBufferSize);
		require(ValidateUniformBufferReferences(Layout, References));
		return GDynamicRHI ? GDynamicRHI->RHICreateUniformBuffer(Layout, Usage, InitialData, References)
			: TRefCountPtr<FRHIUniformBuffer>(new FRHIUniformBuffer(Layout, Usage, InitialData, References));
	}

	FRHIUniformBuffer::FRHIUniformBuffer(const FRHIUniformBufferLayout& InLayout,
		ERHIBufferLifetimeUsage Usage, FByteView InitialData, std::span<FRHIResource* const> References)
		: FRHIResource(ERHIResourceType::UniformBuffer), Layout(InLayout), LifetimeUsage(Usage),
		PendingData(FRHIBufferUploadData::Copy(InitialData))
	{
		SetResourceTable(References);
		for (auto* Reference : References) PendingReferences.emplace_back(Reference);
	}

	auto FRHIUniformBuffer::SetResourceTable(std::span<FRHIResource* const> References) -> void
	{
		require(Layout.Resources.empty() || Layout.Resources.size() == References.size());
		ResourceTable.clear();
		for (auto* Reference : References) ResourceTable.emplace_back(Reference);
	}

	auto FRHIUniformBuffer::UpdateContents(FByteView Data, std::span<FRHIResource* const> References) -> void
	{
		require(IsExecutingRHICommands());
		PendingData = FRHIBufferUploadData::Copy(Data);
		PendingReferences.clear();
		for (auto* Reference : References) PendingReferences.emplace_back(Reference);
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
		require(ValidateUniformBufferReferences(Buffer->GetLayout(), References, Buffer));
		Buffer->SetResourceTable(References);
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

}
