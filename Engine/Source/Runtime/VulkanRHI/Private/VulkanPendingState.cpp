#include "VulkanPendingState.h"

#include "VulkanBuffer.h"
#include "VulkanCommandBuffer.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSets.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanRHIPrivate.h"
#include "VulkanTexture.h"
#include "VulkanView.h"
#include "RHIShaderParameterValidationInternal.h"

namespace Durin::VulkanRHI
{
	FVulkanPendingGraphicsState::FVulkanPendingGraphicsState(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	auto FVulkanPendingGraphicsState::SetGraphicsPipelineState(FVulkanGraphicsPipelineState& InPipelineState, vk::CommandBuffer InCmdBuffer) -> void
	{
		CurrentPipelineState = &InPipelineState;
		CurrentDescriptorState = &FindOrAddDescriptorState(InPipelineState);
		CurrentPipelineState->Bind(InCmdBuffer);
	}

	auto FVulkanPendingGraphicsState::SetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		float MaxDepth = MinZ == MaxZ ? MinZ + 1.0f : MaxZ;

		Viewport
			.setX(MinX)
			.setY(MinY)
			.setWidth(MaxX - MinX)
			.setHeight(MaxY - MinY)
			.setMinDepth(MinZ)
			.setMaxDepth(MaxDepth);

		// Match the common RHI behavior where setting viewport also restores a full-viewport scissor.
		SetScissorRect(static_cast<uint32>(MinX), static_cast<uint32>(MinY), static_cast<uint32>(MaxX - MinX), static_cast<uint32>(MaxY - MinY));
	}

	auto FVulkanPendingGraphicsState::SetScissor(float MinX, float MinY, float Width, float Height) -> void
	{
		SetScissorRect(static_cast<uint32>(MinX), static_cast<uint32>(MinY), static_cast<uint32>(Width), static_cast<uint32>(Height));
	}

	auto FVulkanPendingGraphicsState::SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
	{
		// Shader resource state is scoped to the currently bound PSO descriptor state.
		check(CurrentDescriptorState);
		check(CurrentPipelineState);
		check(InShader);
		const FGraphicsPipelineStateKey& Key = CurrentPipelineState->GetKey();
		EShaderStageFlags ShaderStage = EShaderStageFlags::None;
		if (InShader->GetFrequency() == EShaderFrequency::Vertex
			&& InShader->GetHash() == Key.VertexShaderHash)
			ShaderStage = EShaderStageFlags::Vertex;
		else if (InShader->GetFrequency() == EShaderFrequency::Fragment
			&& InShader->GetHash() == Key.FragmentShaderHash)
			ShaderStage = EShaderStageFlags::Fragment;
		checkf(ShaderStage != EShaderStageFlags::None,
			"Shader parameter update does not belong to the active graphics pipeline.");
		std::string Error;
		checkf(ValidateShaderParameterUpdate(Key.PipelineLayout, ShaderStage,
			InResourceParameters, Error), "Invalid shader parameter update: {}", Error);
		CurrentDescriptorState->SetShaderParameters(InShader, InResourceParameters);
	}

	auto FVulkanGraphicsPipelineDescriptorState::SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
	{
		for (const auto& ResourceParameter : InResourceParameters)
		{
			const auto FoundIt = std::ranges::find_if(PendingShaderResources, [&ResourceParameter](const FRHIShaderParameterResource& ExistingParameter) {
				return ExistingParameter.SetIndex == ResourceParameter.SetIndex
					&& ExistingParameter.BindingIndex == ResourceParameter.BindingIndex
					&& ExistingParameter.ArrayElement == ResourceParameter.ArrayElement;
			});

			if (FoundIt == PendingShaderResources.end())
			{
				PendingShaderResources.push_back(ResourceParameter);
			}
			else
			{
				*FoundIt = ResourceParameter;
			}
		}
		std::vector<TRefCountPtr<FRHIResource>> NewResourceOwners;
		NewResourceOwners.reserve(PendingShaderResources.size());
		for (FRHIShaderParameterResource& Resource : PendingShaderResources)
		{
			NewResourceOwners.emplace_back(Resource.Resource);
			Resource.Resource = NewResourceOwners.back().GetReference();
		}
		PendingResourceOwners = std::move(NewResourceOwners);
	}

	auto FVulkanPendingGraphicsState::PrepareForDraw(FVulkanCommandListContext& InContext) -> void
	{
		check(CurrentPipelineState);
		check(CurrentDescriptorState);

		FVulkanCommandBuffer* CmdBuffer = InContext.GetCommandBuffer();
		CmdBuffer->GetHandle().setViewport(0, Viewport);
		CmdBuffer->GetHandle().setScissor(0, Scissor);

		FVulkanGraphicsPipelineDescriptorState::FDescriptorSetsForDraw DescriptorSetsForDraw = CurrentDescriptorState->GetOrCreateDescriptorSetsForDraw(Device, *CurrentPipelineState);
		if (DescriptorSetsForDraw.DescriptorSets && !DescriptorSetsForDraw.DescriptorSets->empty())
		{
			CmdBuffer->GetHandle().bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,
				CurrentPipelineState->GetPipelineLayout(),
				0,
				*DescriptorSetsForDraw.DescriptorSets,
				DescriptorSetsForDraw.DynamicOffsets
			);
		}
	}

	auto FVulkanPendingGraphicsState::ClearDescriptorSetCache() -> void
	{
		for (const auto& Entry : PipelineStates)
		{
			Entry.second->ClearDescriptorSetCache();
		}
		VerifyDescriptorCacheOccupancy();
	}

	auto FVulkanPendingGraphicsState::NotifyDeletedPipeline(
		FVulkanGraphicsPipelineState* PipelineState) -> void
	{
		if (CurrentPipelineState == PipelineState)
		{
			CurrentPipelineState = nullptr;
			CurrentDescriptorState = nullptr;
		}
		if (const auto It = PipelineStates.find(PipelineState);
			It != PipelineStates.end())
		{
			delete It->second;
			PipelineStates.erase(It);
			VerifyDescriptorCacheOccupancy();
		}
	}

	auto FVulkanPendingGraphicsState::Reset() -> void
	{
		CurrentPipelineState = nullptr;
		CurrentDescriptorState = nullptr;
		// PipelineStates owns its values; keys are non-owning PSO pointers.
		for (const auto& State : PipelineStates | std::views::values)
		{
			delete State;
		}
		PipelineStates.clear();
		VerifyDescriptorCacheOccupancy();
	}

	auto FVulkanGraphicsPipelineDescriptorState::ClearDescriptorSetCache() -> void
	{
		uint64 ValueCount = 0;
		for (const auto& Entry : DescriptorSetCache) ValueCount += Entry.Resources.size();
		Owner.RemoveDescriptorCacheOccupancy(DescriptorSetCache.size(), ValueCount);
		DescriptorSetCache.clear();
		DescriptorSetCacheIndex.clear();
	}

	auto FVulkanGraphicsPipelineDescriptorState::Reset() -> void
	{
		PendingShaderResources.clear();
		PendingResourceOwners.clear();
		ClearDescriptorSetCache();
	}

	auto FVulkanPendingGraphicsState::SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void
	{
		Scissor
			.setOffset({static_cast<int32>(MinX), static_cast<int32>(MinY)})
			.setExtent({Width, Height});
	}

	auto FVulkanPendingGraphicsState::FindOrAddDescriptorState(FVulkanGraphicsPipelineState& InPipelineState) -> FVulkanGraphicsPipelineDescriptorState&
	{
		if (const auto FoundIt = PipelineStates.find(&InPipelineState); FoundIt != PipelineStates.end())
		{
			return *FoundIt->second;
		}

		FVulkanGraphicsPipelineDescriptorState* NewDescriptorState = new FVulkanGraphicsPipelineDescriptorState(*this);
		PipelineStates.emplace(&InPipelineState, NewDescriptorState);
		return *NewDescriptorState;
	}

	static auto SortDescriptorResources(std::vector<FRHIShaderParameterResource>& Resources) -> void
	{
		// Deterministic ordering makes descriptor hashes independent of shader parameter update order.
		std::ranges::sort(Resources, [](const FRHIShaderParameterResource& A, const FRHIShaderParameterResource& B) {
			if (A.SetIndex != B.SetIndex)
			{
				return A.SetIndex < B.SetIndex;
			}
			if (A.BindingIndex != B.BindingIndex)
				return A.BindingIndex < B.BindingIndex;
			return A.ArrayElement < B.ArrayElement;
		});
	}

	auto FVulkanGraphicsPipelineDescriptorState::CalculatePendingDescriptorHash() const -> uint64
	{
		FXxHash64Builder HashBuilder;
		for (const FRHIShaderParameterResource& Resource : PendingShaderResources)
		{
			HashBuilder.UpdateValue(Resource.SetIndex);
			HashBuilder.UpdateValue(Resource.BindingIndex);
			HashBuilder.UpdateValue(Resource.ArrayElement);
			HashBuilder.UpdateValue(Resource.Type);
			HashBuilder.UpdateValue(reinterpret_cast<uintptr_t>(Resource.Resource));
			HashBuilder.UpdateValue(Resource.Size);
			if (Resource.Type != ERHIBindingType::UniformBufferDynamic)
			{
				HashBuilder.UpdateValue(Resource.Offset);
			}
		}
		return HashBuilder.Finalize().HashValue;
	}

	auto FVulkanGraphicsPipelineDescriptorState::AreDescriptorResourcesEqual(
		const std::vector<FRHIShaderParameterResource>& A,
		const std::vector<FRHIShaderParameterResource>& B
	) -> bool
	{
		if (A.size() != B.size())
		{
			return false;
		}

		for (size_t Index = 0; Index < A.size(); ++Index)
		{
			if (A[Index].Resource != B[Index].Resource
				|| A[Index].SetIndex != B[Index].SetIndex
				|| A[Index].BindingIndex != B[Index].BindingIndex
				|| A[Index].ArrayElement != B[Index].ArrayElement
				|| A[Index].Type != B[Index].Type
				|| A[Index].Size != B[Index].Size)
			{
				return false;
			}
			if (A[Index].Type != ERHIBindingType::UniformBufferDynamic && A[Index].Offset != B[Index].Offset)
			{
				return false;
			}
		}
		return true;
	}

	auto FVulkanGraphicsPipelineDescriptorState::GetOrCreateDescriptorSetsForDraw(FVulkanDevice& Device, FVulkanGraphicsPipelineState& PipelineState) -> FDescriptorSetsForDraw
	{
		const FVulkanDescriptorSetsLayout& DescriptorSetsLayout = PipelineState.GetDescriptorSetsLayout();
		const std::vector<vk::DescriptorSetLayout>& LayoutHandles = DescriptorSetsLayout.GetLayoutHandles();
		if (LayoutHandles.empty())
		{
			return {};
		}

		SortDescriptorResources(PendingShaderResources);
		std::string CompletenessError;
		std::vector<uint32> DynamicOffsets;
		DynamicOffsets.reserve(PendingShaderResources.size());
		uint64 BindingValidationVisits = 0;
		const bool bBindingsValid = RHIShaderParameterValidationInternal::VisitOrderedBindings(
			PipelineState.GetKey().PipelineLayout, PendingShaderResources,
			[&](const RHIShaderParameterValidationInternal::FBindingElement& Element,
				const FRHIShaderParameterResource& ResourceRecord) {
					const FBindingLayoutItem& Binding = *Element.Binding;
					const FRHIResource* Resource = ResourceRecord.Resource;
					if (Binding.Type == ERHIBindingType::UniformBuffer
						|| Binding.Type == ERHIBindingType::UniformBufferDynamic
						|| Binding.Type == ERHIBindingType::StorageBuffer)
					{
						checkf(Resource->GetResourceType() == ERHIResourceType::BufferView,
							"Buffer descriptor requires a canonical buffer view.");
						const auto* View = static_cast<const FRHIBufferView*>(Resource);
						const bool bUniform = Binding.Type != ERHIBindingType::StorageBuffer;
						checkf(bUniform
								? View->GetDesc().Type == ERHIBufferViewType::Uniform
								: View->GetDesc().Type == ERHIBufferViewType::StructuredStorage
									|| View->GetDesc().Type == ERHIBufferViewType::ByteAddressStorage,
							"Buffer descriptor view usage is incompatible with its binding type.");
						if (Binding.Type == ERHIBindingType::UniformBufferDynamic)
						{
							const uint64 Alignment = Device.GetGpuProperties().limits
								.minUniformBufferOffsetAlignment;
							checkf(Alignment == 0 || (ResourceRecord.Offset % Alignment) == 0,
								"Dynamic uniform offset is not device-aligned.");
							DynamicOffsets.push_back(ResourceRecord.Offset);
						}
					}
					else if (Binding.Type == ERHIBindingType::Texture
						|| Binding.Type == ERHIBindingType::StorageImage)
					{
						checkf(Resource->GetResourceType() == ERHIResourceType::TextureView,
							"Image descriptor requires a canonical texture view.");
						const auto* View = static_cast<const FRHITextureView*>(Resource);
						checkf(Binding.Type == ERHIBindingType::Texture
								? View->GetDesc().Usage == ERHITextureViewUsage::Sampled
								: View->GetDesc().Usage == ERHITextureViewUsage::Storage,
							"Image descriptor view usage is incompatible with its binding type.");
						const auto* VulkanTexture = static_cast<const FVulkanTexture*>(
							View->GetTexture());
						const ERHIAccess ExpectedAccess = Binding.Type == ERHIBindingType::Texture
							? ERHIAccess::GraphicsShaderRead
							: ERHIAccess::GraphicsShaderReadWrite;
						ERHIAccess TrackedAccess = ERHIAccess::None;
						checkf(ValidateVulkanTextureDescriptorState(
							VulkanTexture->GetStateTracker(), View->GetDesc().Range,
							Binding.Type, TrackedAccess),
							"Image descriptor binding state mismatch: set={}, binding={}, element={}, type={}, expectedAccess={}, trackedAccess={}.",
							Element.SetIndex, Binding.Slot, Element.ArrayElement,
							static_cast<uint32>(Binding.Type), static_cast<uint32>(ExpectedAccess),
							static_cast<uint32>(TrackedAccess));
					}
					else
						checkf(Resource->GetResourceType() == ERHIResourceType::Sampler,
							"Sampler descriptor requires a sampler resource.");
				}, CompletenessError, &BindingValidationVisits);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanBindingValidationVisitCount.fetch_add(
			BindingValidationVisits, std::memory_order_relaxed);
#endif
		checkf(bBindingsValid, "Invalid shader binding snapshot: {}", CompletenessError);
		const uint64 DescriptorHash = CalculatePendingDescriptorHash();

		// Hash is a fast reject only; resource equality is still checked before cache reuse.
		const auto [FirstCandidate, LastCandidate] = DescriptorSetCacheIndex.equal_range(DescriptorHash);
		for (auto Candidate = FirstCandidate; Candidate != LastCandidate; ++Candidate)
		{
			FVulkanDescriptorSetCacheEntry& Entry = DescriptorSetCache[Candidate->second];
			if (AreDescriptorResourcesEqual(Entry.Resources, PendingShaderResources))
			{
				++Device.GetGraphicsCacheStatisticsMutable().DescriptorSnapshots.Hits;
				Owner.TouchDescriptorCacheEntry(Entry);
				return FDescriptorSetsForDraw{&Entry.DescriptorSets, std::move(DynamicOffsets)};
			}
		}
		++Device.GetGraphicsCacheStatisticsMutable().DescriptorSnapshots.Misses;

		FVulkanDescriptorSetCacheEntry NewEntry;
		NewEntry.Hash = DescriptorHash;
		NewEntry.Resources = PendingShaderResources;
		NewEntry.ResourceOwners.reserve(NewEntry.Resources.size());
		for (FRHIShaderParameterResource& Resource : NewEntry.Resources)
		{
			NewEntry.ResourceOwners.emplace_back(Resource.Resource);
			Resource.Resource = NewEntry.ResourceOwners.back().GetReference();
		}

		NewEntry.DescriptorSets = Device.GetGlobalDescriptorPool().AllocateDescriptorSets(
			LayoutHandles,
			DescriptorSetsLayout.GetInfo().GetDescriptorRequirements()
		);

		// Vulkan write descriptors store pointers into these arrays until updateDescriptorSets returns.
		std::vector<vk::DescriptorBufferInfo> BufferInfos;
		std::vector<vk::DescriptorImageInfo> ImageInfos;
		std::vector<vk::WriteDescriptorSet> DescriptorWrites;
		BufferInfos.reserve(NewEntry.Resources.size());
		ImageInfos.reserve(NewEntry.Resources.size());
		DescriptorWrites.reserve(NewEntry.Resources.size());

		for (const FRHIShaderParameterResource& Resource : NewEntry.Resources)
		{
			if (Resource.Resource == nullptr)
			{
				continue;
			}
			check(Resource.SetIndex < NewEntry.DescriptorSets.size());

			vk::WriteDescriptorSet DescriptorWrite{};
			DescriptorWrite
				.setDstSet(NewEntry.DescriptorSets[Resource.SetIndex])
				.setDstBinding(Resource.BindingIndex)
				.setDstArrayElement(Resource.ArrayElement)
				.setDescriptorType(ToVulkan_RHIBindingType(Resource.Type))
				.setDescriptorCount(1);

			switch (Resource.Type)
			{
			case ERHIBindingType::UniformBuffer:
			case ERHIBindingType::UniformBufferDynamic:
			case ERHIBindingType::StorageBuffer:
				{
					const auto* View = static_cast<const FVulkanBufferView*>(Resource.Resource);
					const auto* Buffer = static_cast<const FVulkanBuffer*>(View->GetBuffer());
					const FRHIBufferViewDesc& ViewDesc = View->GetDesc();
					if (Resource.Type == ERHIBindingType::StorageBuffer)
					{
						checkf(
							EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ByteAddressBuffer | EBufferUsageFlags::ShaderResource),
							"A storage buffer descriptor requires a buffer created with shader-storage usage");
					}
					vk::DescriptorBufferInfo& BufferInfo = BufferInfos.emplace_back();
					BufferInfo
						.setBuffer(Buffer->GetHandle())
						.setOffset(ViewDesc.Offset)
						.setRange(ViewDesc.Size);
					DescriptorWrite.setBufferInfo(BufferInfo);
					break;
				}
			case ERHIBindingType::Texture:
				{
					const auto* View = static_cast<const FVulkanTextureView*>(Resource.Resource);
					vk::DescriptorImageInfo& ImageInfo = ImageInfos.emplace_back();
					ImageInfo
						.setImageView(View->GetHandle())
						.setImageLayout(GetVulkanDescriptorImageLayout(Resource.Type));
					DescriptorWrite.setImageInfo(ImageInfo);
					break;
				}
			case ERHIBindingType::StorageImage:
				{
					const auto* View = static_cast<const FVulkanTextureView*>(Resource.Resource);
					const auto* Texture = static_cast<const FVulkanTexture*>(View->GetTexture());
					checkf(EnumHasAnyFlags(Texture->CreateFlags, ETextureCreateFlags::Storage), "A storage image descriptor requires a texture created with ETextureCreateFlags::Storage");
					vk::DescriptorImageInfo& ImageInfo = ImageInfos.emplace_back();
					ImageInfo
						.setImageView(View->GetHandle())
						.setImageLayout(GetVulkanDescriptorImageLayout(Resource.Type));
					DescriptorWrite.setImageInfo(ImageInfo);
					break;
				}
			case ERHIBindingType::Sampler:
				{
					const FVulkanSampler* Sampler = static_cast<const FVulkanSampler*>(Resource.Resource);
					vk::DescriptorImageInfo& ImageInfo = ImageInfos.emplace_back();
					ImageInfo.setSampler(Sampler->GetHandle());
					DescriptorWrite.setImageInfo(ImageInfo);
					break;
				}
			default:
				checkf(false, "Unsupported shader parameter resource type: {}", static_cast<uint32>(Resource.Type));
				break;
			}

			DescriptorWrites.push_back(DescriptorWrite);
		}

		Device.GetHandle().updateDescriptorSets(DescriptorWrites, {});
		++Device.GetGraphicsCacheStatisticsMutable().DescriptorSnapshots.NativeCreations;
		++Device.GetGraphicsCacheStatisticsMutable().DescriptorAllocations;
		DescriptorSetCache.push_back(std::move(NewEntry));
		DescriptorSetCacheIndex.emplace(DescriptorHash, DescriptorSetCache.size() - 1);
		FVulkanDescriptorSetCacheEntry& CommittedEntry = DescriptorSetCache.back();
		Owner.AddDescriptorCacheOccupancy(1, CommittedEntry.Resources.size());
		Owner.TouchDescriptorCacheEntry(CommittedEntry);
		Owner.EnforceDescriptorCacheBudget();
		for (FVulkanDescriptorSetCacheEntry& Entry : DescriptorSetCache)
		{
			if (Entry.LastUsed == Owner.DescriptorAccessSerial)
				return FDescriptorSetsForDraw{&Entry.DescriptorSets, std::move(DynamicOffsets)};
		}
		checkf(false, "A newly created descriptor snapshot exceeded the configured cache budget.");
		return {};
	}

	auto FVulkanPendingGraphicsState::TouchDescriptorCacheEntry(
		FVulkanGraphicsPipelineDescriptorState::FVulkanDescriptorSetCacheEntry& Entry) -> void
	{
		Entry.LastUsed = ++DescriptorAccessSerial;
	}

	auto FVulkanPendingGraphicsState::AddDescriptorCacheOccupancy(
		uint64 EntryCount, uint64 ValueCount) -> void
	{
		check(DescriptorEntryOccupancy <= std::numeric_limits<uint64>::max() - EntryCount);
		check(DescriptorValueOccupancy <= std::numeric_limits<uint64>::max() - ValueCount);
		DescriptorEntryOccupancy += EntryCount;
		DescriptorValueOccupancy += ValueCount;
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable();
		Stats.DescriptorSnapshots.Occupancy = DescriptorEntryOccupancy;
		Stats.DescriptorValueOccupancy = DescriptorValueOccupancy;
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanDescriptorOccupancyMutationCount.fetch_add(1, std::memory_order_relaxed);
#endif
	}

	auto FVulkanPendingGraphicsState::RemoveDescriptorCacheOccupancy(
		uint64 EntryCount, uint64 ValueCount) -> void
	{
		check(EntryCount <= DescriptorEntryOccupancy);
		check(ValueCount <= DescriptorValueOccupancy);
		DescriptorEntryOccupancy -= EntryCount;
		DescriptorValueOccupancy -= ValueCount;
		auto& Stats = Device.GetGraphicsCacheStatisticsMutable();
		Stats.DescriptorSnapshots.Occupancy = DescriptorEntryOccupancy;
		Stats.DescriptorValueOccupancy = DescriptorValueOccupancy;
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		if (EntryCount != 0 || ValueCount != 0)
			GVulkanDescriptorOccupancyMutationCount.fetch_add(1, std::memory_order_relaxed);
#endif
	}

	auto FVulkanPendingGraphicsState::VerifyDescriptorCacheOccupancy() const -> void
	{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		uint64 EntryCount = 0;
		uint64 ValueCount = 0;
		for (const auto* State : PipelineStates | std::views::values)
		{
			GVulkanDescriptorOccupancyVerificationVisitCount.fetch_add(
				1, std::memory_order_relaxed);
			EntryCount += State->DescriptorSetCache.size();
			for (const auto& Entry : State->DescriptorSetCache)
			{
				GVulkanDescriptorOccupancyVerificationVisitCount.fetch_add(
					1, std::memory_order_relaxed);
				ValueCount += Entry.Resources.size();
			}
		}
		checkfSlow(EntryCount == DescriptorEntryOccupancy
			&& ValueCount == DescriptorValueOccupancy,
			"Incremental descriptor occupancy diverged from owned cache state.");
#endif
	}

	auto FVulkanPendingGraphicsState::EnforceDescriptorCacheBudget() -> void
	{
		while (true)
		{
			if (DescriptorEntryOccupancy <= 512 && DescriptorValueOccupancy <= 8192)
				break;
			FVulkanGraphicsPipelineDescriptorState* VictimState = nullptr;
			size_t VictimIndex = 0;
			uint64 Oldest = std::numeric_limits<uint64>::max();
			for (auto* State : PipelineStates | std::views::values)
			{
				for (size_t Index = 0; Index < State->DescriptorSetCache.size(); ++Index)
				{
					const auto& Entry = State->DescriptorSetCache[Index];
					if (Entry.LastUsed < Oldest)
					{
						Oldest = Entry.LastUsed;
						VictimState = State;
						VictimIndex = Index;
					}
				}
			}
			check(VictimState);
			RemoveDescriptorCacheOccupancy(1,
				VictimState->DescriptorSetCache[VictimIndex].Resources.size());
			VictimState->DescriptorSetCache.erase(VictimState->DescriptorSetCache.begin() + VictimIndex);
			VictimState->RebuildCacheIndex();
			++Device.GetGraphicsCacheStatisticsMutable().DescriptorSnapshots.Evictions;
		}
	}

	auto FVulkanGraphicsPipelineDescriptorState::RebuildCacheIndex() -> void
	{
		DescriptorSetCacheIndex.clear();
		for (size_t Index = 0; Index < DescriptorSetCache.size(); ++Index)
			DescriptorSetCacheIndex.emplace(DescriptorSetCache[Index].Hash, Index);
	}
} // namespace Durin::VulkanRHI
