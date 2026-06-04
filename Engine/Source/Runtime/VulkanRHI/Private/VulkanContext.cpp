#include "VulkanContext.h"

#include "RHIResources.h"

#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanRenderPass.h"
#include "VulkanViewport.h"
#include "VulkanQueue.h"
#include "VulkanSubmission.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanCommandListContext::FVulkanCommandListContext(FVulkanDynamicRHI* InRHI, FVulkanDevice& InDevice, FVulkanQueue* InQueue)
		: RHI(InRHI)
		, Device(InDevice)
		, Queue(InQueue)
	{
		Pool = new FVulkanCommandBufferPool(Device);
		Pool->CreatePool(Queue->GetFamilyIndex());
	}

	FVulkanCommandListContext::~FVulkanCommandListContext()
	{
		delete Pool;
	}

	auto FVulkanCommandListContext::RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		PendingGfxPipelineState->SetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
	}

	auto FVulkanCommandListContext::RHISetScissor(float MinX, float MinY, float Width, float Height) -> void
	{
		PendingGfxPipelineState->SetScissor(MinX, MinY, Width, Height);
	}

	auto FVulkanCommandListContext::RHIBeginFrame() -> void
	{
		DescriptorSetCache.clear();
	}

	auto FVulkanCommandListContext::RHIEndFrame() -> void
	{
		FVulkanFrame& Frame = Device.GetCurrentFrame();
		FVulkanPayload& Payload = GetPayload();
		Payload.Fence = Frame.GetFrameFence();
		Finalize();

		Pool->FreeUnusedCommandBuffers(Queue);
	}

	auto FVulkanCommandListContext::RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName Name) -> void
	{
		const auto* VulkanRT = static_cast<FVulkanTexture*>(RenderPassInfo.ColorRenderTargets[0]);

		FVulkanRenderPassManager& RenderPassManager = Device.GetRenderPassManager();
		FVulkanRenderPass* RenderPass = Device.GetRenderPassManager().GetOrCreateRenderPass(Name, VulkanRT->Format);

		FRHIRenderTargetsInfo RTInfo{};
		RTInfo.NumColorRenderTargets = 1;
		RTInfo.ColorRenderTargets[0] = RenderPassInfo.ColorRenderTargets[0];
		FVulkanFramebuffer* Framebuffer = RenderPassManager.GetOrCreateFrameBuffer(RTInfo);
		RenderPassManager.BeginRenderPass(*this, Device, GetCommandBuffer(), RenderPassInfo, RenderPass, Framebuffer);
	}

	auto FVulkanCommandListContext::RHIEndRenderPass() -> void
	{
		Device.GetRenderPassManager().EndRenderPass(GetCommandBuffer());
	}

	auto FVulkanCommandListContext::RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void
	{
	}

	auto FVulkanCommandListContext::RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
	{
		if (!bPresent)
		{
			return;
		}

		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport);

		FVulkanQueue* PresentQueue = Device.GetPresentQueue();
		VulkanViewport->Present(*this, *GetCommandBuffer(), *PresentQueue, bLockToVsync);
	}

	auto FVulkanCommandListContext::RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void
	{
		PendingGfxPipelineState = static_cast<FVulkanGraphicsPipelineState*>(&GraphicsPipelineState);
		PendingGfxPipelineState->Bind(GetCommandBuffer()->GetHandle());
	}

	auto FVulkanCommandListContext::RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* InVertexBuffer, uint32 Offset) -> void
	{
		if (InVertexBuffer != nullptr)
		{
			vk::Buffer BufferHandle = static_cast<FVulkanBuffer*>(InVertexBuffer)->GetHandle();
			GetCommandBuffer()->GetHandle().bindVertexBuffers(StreamIndex, BufferHandle, {Offset});
		}
	}

	static constexpr auto DeduceIndexType(uint32 Stride) -> vk::IndexType
	{
		if (Stride == 2) { return vk::IndexType::eUint16; }
		if (Stride == 4) { return vk::IndexType::eUint32; }
		checkf(false, "Unsupported index buffer stride: {}", Stride);
		return vk::IndexType::eUint16;
	}

	auto FVulkanCommandListContext::RHIBindIndexBuffer(FRHIBuffer* InIndexBuffer, uint32 Offset) -> void
	{
		if (InIndexBuffer != nullptr)
		{
			const FVulkanBuffer* IndexBuffer = static_cast<FVulkanBuffer*>(InIndexBuffer);
			GetCommandBuffer()->GetHandle().bindIndexBuffer(IndexBuffer->GetHandle(), Offset, DeduceIndexType(IndexBuffer->GetStride()));
		}
	}

	auto FVulkanCommandListContext::RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void
	{
		PendingGfxPipelineState->PushConstants(*this, StageFlags, Offset, Size, Data);
	}

	auto FVulkanCommandListContext::RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void
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

	auto FVulkanCommandListContext::RHIDrawIndexed(uint32 IndexCount, uint32 StartIndexLocation, int32 VertexOffset) -> void
	{
		PendingGfxPipelineState->PrepareForDraw(*this);
		const std::vector<vk::DescriptorSet>& DescriptorSets = GetOrCreateDescriptorSetsForDraw();
		if (!DescriptorSets.empty())
		{
			GetCommandBuffer()->GetHandle().bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,
				PendingGfxPipelineState->GetPipelineLayout(),
				0,
				DescriptorSets,
				{}
			);
		}
		GetCommandBuffer()->GetHandle().drawIndexed(IndexCount, 1, StartIndexLocation, VertexOffset, 0);
	}

	auto FVulkanCommandListContext::GetCommandBuffer() -> FVulkanCommandBuffer*
	{
		FVulkanPayload& Payload = GetPayload();
		if (Payload.CommandBuffers.empty())
		{
			PrepareNewCommandBuffer(Payload);
		}
		return Payload.CommandBuffers.back();
	}

	auto FVulkanCommandListContext::RHISetShaderUniformBuffer(FRHIShader* InShader, uint32 SetIndex, uint32 BindIndex, FRHIBuffer* InUniformBuffer) -> void
	{
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

	auto FVulkanCommandListContext::CalculatePendingDescriptorHash(uint64 LayoutHash) const -> uint64
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

	auto FVulkanCommandListContext::AreDescriptorResourcesEqual(
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

	auto FVulkanCommandListContext::GetOrCreateDescriptorSetsForDraw() -> const std::vector<vk::DescriptorSet>&
	{
		check(PendingGfxPipelineState);

		const FVulkanDescriptorSetsLayout& DescriptorSetsLayout = PendingGfxPipelineState->GetDescriptorSetsLayout();
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

	auto FVulkanCommandListContext::AddWaitSemaphore(vk::PipelineStageFlags InWaitFlag, FVulkanSemaphore* InWaitSemaphore) -> void
	{
		AddWaitSemaphores(InWaitFlag, std::span(&InWaitSemaphore, 1));
	}

	auto FVulkanCommandListContext::AddWaitSemaphores(vk::PipelineStageFlags InWaitFlag, std::span<FVulkanSemaphore*> InWaitSemaphores) -> void
	{
		auto& Payload = GetPayload();
		Payload.WaitSemaphores.insert(Payload.WaitSemaphores.end(), InWaitSemaphores.begin(), InWaitSemaphores.end());
		Payload.WaitFlags.insert(Payload.WaitFlags.end(), InWaitSemaphores.size(), InWaitFlag);
	}

	auto FVulkanCommandListContext::AddSignalSemaphore(FVulkanSemaphore* InSignalSemaphore) -> void
	{
 		AddSignalSemaphores(std::span(&InSignalSemaphore, 1));
	}

	auto FVulkanCommandListContext::AddSignalSemaphores(std::span<FVulkanSemaphore*> InSignalSemaphores) -> void
	{
		auto& Payload = GetPayload();
		Payload.SignalSemaphores.insert(Payload.SignalSemaphores.end(), InSignalSemaphores.begin(), InSignalSemaphores.end());
	}

	auto FVulkanCommandListContext::NotifyDeleted_Image(vk::Image Image) -> void
	{
	}

	auto FVulkanCommandListContext::Finalize() -> void
	{
		GetCommandBuffer()->End();
		Queue->SubmitPayloads(Payloads);
		Payloads.clear();
	}

	auto FVulkanCommandListContext::PrepareNewCommandBuffer(FVulkanPayload& InPayload) -> void
	{
		check(InPayload.CommandBuffers.empty());
		FVulkanCommandBuffer* NewCmdBuffer = Pool->Create();
		InPayload.CommandBuffers.push_back(NewCmdBuffer);
		NewCmdBuffer->Begin();
	}

	auto FVulkanCommandListContext::GetPayload() -> FVulkanPayload&
	{
		// Currently only support one payload per submit.
		if (Payloads.empty())
		{
			Payloads.push_back(new FVulkanPayload(*Queue));
		}
		return *Payloads.back();
	}

	auto FVulkanDynamicRHI::RHIGetDefaultContext() -> IRHICommandContext*
	{
		return Device->GetImmediateContext();
	}

	auto FVulkanDynamicRHI::RHIGetCommandContext(ERHIPipeline Pipeline) const -> IRHICommandContext*
	{
		if (Pipeline != ERHIPipeline::Graphics)
		{
			return nullptr;
		}

		return Device->GetImmediateContext();
	}
} // namespace Durin::VulkanRHI
