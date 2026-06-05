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
		PipelineState = &InPipelineState;
		PipelineState->Bind(InCmdBuffer);
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

		SetScissorRect(static_cast<uint32>(MinX), static_cast<uint32>(MinY), static_cast<uint32>(MaxX - MinX), static_cast<uint32>(MaxY - MinY));
	}

	auto FVulkanPendingGraphicsState::SetScissor(float MinX, float MinY, float Width, float Height) -> void
	{
		SetScissorRect(static_cast<uint32>(MinX), static_cast<uint32>(MinY), static_cast<uint32>(Width), static_cast<uint32>(Height));
	}

	auto FVulkanPendingGraphicsState::SetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
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
		check(PipelineState);

		FVulkanCommandBuffer* CmdBuffer = InContext.GetCommandBuffer();
		CmdBuffer->GetHandle().setViewport(0, Viewport);
		CmdBuffer->GetHandle().setScissor(0, Scissor);

		const std::vector<vk::DescriptorSet>& DescriptorSets = GetOrCreateDescriptorSetsForDraw();
		if (!DescriptorSets.empty())
		{
			CmdBuffer->GetHandle().bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,
				PipelineState->GetPipelineLayout(),
				0,
				DescriptorSets,
				{}
			);
		}
	}

	auto FVulkanPendingGraphicsState::ClearDescriptorSetCache() -> void
	{
		DescriptorSetCache.clear();
	}

	auto FVulkanPendingGraphicsState::Reset() -> void
	{
		PipelineState = nullptr;
		PendingShaderResources.clear();
		DescriptorSetCache.clear();
	}

	auto FVulkanPendingGraphicsState::SetScissorRect(uint32 MinX, uint32 MinY, uint32 Width, uint32 Height) -> void
	{
		Scissor
			.setOffset({static_cast<int32>(MinX), static_cast<int32>(MinY)})
			.setExtent({Width, Height});
	}

	static auto SortDescriptorResources(std::vector<FRHIShaderParameterResource>& Resources) -> void
	{
		std::ranges::sort(Resources, [](const FRHIShaderParameterResource& A, const FRHIShaderParameterResource& B) {
			if (A.SetIndex != B.SetIndex)
			{
				return A.SetIndex < B.SetIndex;
			}
			return A.BindingIndex < B.BindingIndex;
		});
	}

	auto FVulkanPendingGraphicsState::CalculatePendingDescriptorHash(uint64 LayoutHash) const -> uint64
	{
		FXxHash64Builder HashBuilder;
		HashBuilder.UpdateValue(LayoutHash);
		for (const FRHIShaderParameterResource& Resource : PendingShaderResources)
		{
			HashBuilder.UpdateValue(Resource.SetIndex);
			HashBuilder.UpdateValue(Resource.BindingIndex);
			HashBuilder.UpdateValue(Resource.Type);
			HashBuilder.UpdateValue(reinterpret_cast<uintptr_t>(Resource.Resource));
		}
		return HashBuilder.Finalize().HashValue;
	}

	auto FVulkanPendingGraphicsState::AreDescriptorResourcesEqual(
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
				|| A[Index].Type != B[Index].Type)
			{
				return false;
			}
		}
		return true;
	}

	auto FVulkanPendingGraphicsState::GetOrCreateDescriptorSetsForDraw() -> const std::vector<vk::DescriptorSet>&
	{
		check(PipelineState);

		const FVulkanDescriptorSetsLayout& DescriptorSetsLayout = PipelineState->GetDescriptorSetsLayout();
		const std::vector<vk::DescriptorSetLayout>& LayoutHandles = DescriptorSetsLayout.GetLayoutHandles();
		static const std::vector<vk::DescriptorSet> EmptyDescriptorSets;
		if (LayoutHandles.empty())
		{
			return EmptyDescriptorSets;
		}

		SortDescriptorResources(PendingShaderResources);
		const uint64 LayoutHash = DescriptorSetsLayout.GetHash();
		const uint64 DescriptorHash = CalculatePendingDescriptorHash(LayoutHash);

		for (FVulkanDescriptorSetCacheEntry& Entry : DescriptorSetCache)
		{
			if (Entry.Hash == DescriptorHash
				&& Entry.LayoutHash == LayoutHash
				&& AreDescriptorResourcesEqual(Entry.Resources, PendingShaderResources))
			{
				return Entry.DescriptorSets;
			}
		}

		FVulkanDescriptorSetCacheEntry& NewEntry = DescriptorSetCache.emplace_back();
		NewEntry.Hash = DescriptorHash;
		NewEntry.LayoutHash = LayoutHash;
		NewEntry.Resources = PendingShaderResources;

		vk::DescriptorSetAllocateInfo DescriptorSetAllocInfo;
		DescriptorSetAllocInfo
			.setDescriptorPool(Device.GetGlobalDescriptorPool().GetPool())
			.setSetLayouts(LayoutHandles);

		NewEntry.DescriptorSets = Device.GetHandle().allocateDescriptorSets(DescriptorSetAllocInfo);

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
				{
					const FVulkanBuffer* Buffer = static_cast<const FVulkanBuffer*>(Resource.Resource);
					vk::DescriptorBufferInfo& BufferInfo = BufferInfos.emplace_back();
					BufferInfo
						.setBuffer(Buffer->GetHandle())
						.setOffset(0)
						.setRange(Buffer->GetSize());
					DescriptorWrite.setBufferInfo(BufferInfo);
					break;
				}
			case ERHIBindingType::Texture:
				{
					const FVulkanTexture* Texture = static_cast<const FVulkanTexture*>(Resource.Resource);
					vk::DescriptorImageInfo& ImageInfo = ImageInfos.emplace_back();
					ImageInfo
						.setImageView(Texture->ImageView)
						.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
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
		return NewEntry.DescriptorSets;
	}
} // namespace Durin::VulkanRHI
