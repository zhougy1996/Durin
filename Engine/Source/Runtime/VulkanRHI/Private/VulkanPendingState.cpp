#include "VulkanPendingState.h"

#include "VulkanBuffer.h"
#include "VulkanCommandBuffer.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSets.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanRHIPrivate.h"
#include "VulkanTexture.h"

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
		CurrentDescriptorState->SetShaderParameters(InShader, InResourceParameters);
	}

	auto FVulkanGraphicsPipelineDescriptorState::SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
	{
		for (const auto& ResourceParameter : InResourceParameters)
		{
			const auto FoundIt = std::ranges::find_if(PendingShaderResources, [&ResourceParameter](const FRHIShaderParameterResource& ExistingParameter) {
				return ExistingParameter.SetIndex == ResourceParameter.SetIndex
					&& ExistingParameter.BindingIndex == ResourceParameter.BindingIndex;
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
	}

	auto FVulkanGraphicsPipelineDescriptorState::ClearDescriptorSetCache() -> void
	{
		DescriptorSetCache.clear();
	}

	auto FVulkanGraphicsPipelineDescriptorState::Reset() -> void
	{
		PendingShaderResources.clear();
		DescriptorSetCache.clear();
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

		FVulkanGraphicsPipelineDescriptorState* NewDescriptorState = new FVulkanGraphicsPipelineDescriptorState();
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
			return A.BindingIndex < B.BindingIndex;
		});
	}

	auto FVulkanGraphicsPipelineDescriptorState::CalculatePendingDescriptorHash() const -> uint64
	{
		FXxHash64Builder HashBuilder;
		for (const FRHIShaderParameterResource& Resource : PendingShaderResources)
		{
			HashBuilder.UpdateValue(Resource.SetIndex);
			HashBuilder.UpdateValue(Resource.BindingIndex);
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
		const uint64 DescriptorHash = CalculatePendingDescriptorHash();

		std::vector<uint32> DynamicOffsets;
		for (const FRHIShaderParameterResource& Resource : PendingShaderResources)
		{
			if (Resource.Type == ERHIBindingType::UniformBufferDynamic)
			{
				DynamicOffsets.push_back(Resource.Offset);
			}
		}

		// Hash is a fast reject only; resource equality is still checked before cache reuse.
		for (FVulkanDescriptorSetCacheEntry& Entry : DescriptorSetCache)
		{
			if (Entry.Hash == DescriptorHash
				&& AreDescriptorResourcesEqual(Entry.Resources, PendingShaderResources))
			{
				return FDescriptorSetsForDraw{&Entry.DescriptorSets, std::move(DynamicOffsets)};
			}
		}

		FVulkanDescriptorSetCacheEntry& NewEntry = DescriptorSetCache.emplace_back();
		NewEntry.Hash = DescriptorHash;
		NewEntry.Resources = PendingShaderResources;

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
				.setDstArrayElement(0)
				.setDescriptorType(ToVulkan_RHIBindingType(Resource.Type))
				.setDescriptorCount(1);

			switch (Resource.Type)
			{
			case ERHIBindingType::UniformBuffer:
			case ERHIBindingType::UniformBufferDynamic:
			case ERHIBindingType::StorageBuffer:
				{
					const FVulkanBuffer* Buffer = static_cast<const FVulkanBuffer*>(Resource.Resource);
					if (Resource.Type == ERHIBindingType::StorageBuffer)
					{
						checkf(
							EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ByteAddressBuffer | EBufferUsageFlags::ShaderResource),
							"A storage buffer descriptor requires a buffer created with shader-storage usage");
					}
					const bool bDynamicUniform = Resource.Type == ERHIBindingType::UniformBufferDynamic;
					const uint32 DescriptorOffset = bDynamicUniform ? 0 : Resource.Offset;
					checkf(DescriptorOffset <= Buffer->GetSize(), "Descriptor buffer offset exceeds the buffer size");
					vk::DescriptorBufferInfo& BufferInfo = BufferInfos.emplace_back();
					const uint32 Range = Resource.Size != 0 ? Resource.Size : Buffer->GetSize() - DescriptorOffset;
					const uint32 EffectiveOffset = bDynamicUniform ? Resource.Offset : DescriptorOffset;
					checkf(EffectiveOffset <= Buffer->GetSize() && Range > 0 && Range <= Buffer->GetSize() - EffectiveOffset, "Descriptor buffer range exceeds the buffer size");
					BufferInfo
						.setBuffer(Buffer->GetHandle())
						.setOffset(DescriptorOffset)
						.setRange(Range);
					DescriptorWrite.setBufferInfo(BufferInfo);
					break;
				}
			case ERHIBindingType::Texture:
				{
					const FVulkanTexture* Texture = static_cast<const FVulkanTexture*>(Resource.Resource);
					vk::DescriptorImageInfo& ImageInfo = ImageInfos.emplace_back();
					ImageInfo
						.setImageView(Texture->ImageView)
						.setImageLayout(EnumHasAnyFlags(Texture->CreateFlags, ETextureCreateFlags::Storage)
							? vk::ImageLayout::eGeneral
							: vk::ImageLayout::eShaderReadOnlyOptimal);
					DescriptorWrite.setImageInfo(ImageInfo);
					break;
				}
			case ERHIBindingType::StorageImage:
				{
					const FVulkanTexture* Texture = static_cast<const FVulkanTexture*>(Resource.Resource);
					checkf(EnumHasAnyFlags(Texture->CreateFlags, ETextureCreateFlags::Storage), "A storage image descriptor requires a texture created with ETextureCreateFlags::Storage");
					vk::DescriptorImageInfo& ImageInfo = ImageInfos.emplace_back();
					ImageInfo
						.setImageView(Texture->ImageView)
						.setImageLayout(vk::ImageLayout::eGeneral);
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
		return FDescriptorSetsForDraw{&NewEntry.DescriptorSets, std::move(DynamicOffsets)};
	}
} // namespace Durin::VulkanRHI
