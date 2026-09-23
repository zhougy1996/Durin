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

#include <cstdlib>

namespace Durin::VulkanRHI
{
	static auto UpdateDescriptorSets(FVulkanDevice& Device,
		std::span<const FRHIShaderParameterResource> Resources,
		std::span<const vk::DescriptorSet> DescriptorSets, uint32 FirstSet = 0) -> void
	{
		// Vulkan write descriptors store pointers into these arrays until updateDescriptorSets returns.
		std::vector<vk::DescriptorBufferInfo> BufferInfos;
		std::vector<vk::DescriptorImageInfo> ImageInfos;
		std::vector<vk::WriteDescriptorSet> DescriptorWrites;
		BufferInfos.reserve(Resources.size());
		ImageInfos.reserve(Resources.size());
		DescriptorWrites.reserve(Resources.size());

		for (const FRHIShaderParameterResource& Resource : Resources)
		{
			if (Resource.Resource == nullptr)
			{
				continue;
			}
			check(Resource.SetIndex >= FirstSet && Resource.SetIndex - FirstSet < DescriptorSets.size());

			vk::WriteDescriptorSet DescriptorWrite{};
			DescriptorWrite
				.setDstSet(DescriptorSets[Resource.SetIndex - FirstSet])
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
					const auto* View = FVulkanBufferView::Cast(Resource.Resource);
					const auto* Buffer = FVulkanBuffer::Cast(View->GetBuffer());
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
			case ERHIBindingType::StorageImage:
				{
					const auto* View = static_cast<const FVulkanTextureView*>(Resource.Resource);
					if (Resource.Type == ERHIBindingType::StorageImage)
					{
						const auto* Texture = static_cast<const FVulkanTexture*>(View->GetTexture());
						checkf(EnumHasAnyFlags(Texture->CreateFlags, ETextureCreateFlags::Storage),
							"A storage image descriptor requires a texture created with ETextureCreateFlags::Storage");
					}
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
	}

	auto FVulkanPendingComputeState::SetComputePipelineState(
		FVulkanComputePipelineState& InPipelineState,
		vk::CommandBuffer InCmdBuffer) -> void
	{
		if (CurrentPipelineState != &InPipelineState)
		{
			PendingResources.clear();
			DeferredBindings.Clear();
			PendingOwners.clear();
			ClearDescriptorSetCache();
		}
		CurrentPipelineState = &InPipelineState;
		CurrentPipelineState->Bind(InCmdBuffer);
	}

	auto FVulkanPendingComputeState::SetShaderParameters(FRHIShader* InShader,
		std::span<const FRHIShaderParameterResource> InResourceParameters, bool bResolvingDeferred) -> void
	{
		check(CurrentPipelineState && (InShader || bResolvingDeferred));
		if (!bResolvingDeferred)
		{
			DeferredBindings.Update(InResourceParameters);
			const FComputePipelineStateKey& Key = CurrentPipelineState->GetKey();
			checkf(InShader->GetFrequency() == EShaderFrequency::Compute
				&& InShader->GetHash() == Key.ComputeShaderHash,
				"Shader parameter update does not belong to the active compute pipeline.");
#if DO_CHECK
			const auto ValidationResult = ValidateShaderParameterUpdate(Key.PipelineLayout,
				EShaderStageFlags::Compute, InResourceParameters);
			checkf(ValidationResult,
				"Invalid compute shader parameter update: {}", ToString(ValidationResult.error()));
#endif
		}
		for (const FRHIShaderParameterResource& Parameter : InResourceParameters)
		{
			const auto It = std::ranges::find_if(PendingResources,
				[&](const FRHIShaderParameterResource& Existing) {
					return Existing.SetIndex == Parameter.SetIndex
						&& Existing.BindingIndex == Parameter.BindingIndex
						&& Existing.ArrayElement == Parameter.ArrayElement;
				});
			if (It == PendingResources.end()) PendingResources.push_back(Parameter);
			else *It = Parameter;
		}
		PendingOwners.clear();
		PendingOwners.reserve(PendingResources.size());
		for (FRHIShaderParameterResource& Resource : PendingResources)
		{
			PendingOwners.emplace_back(Resource.Resource);
			Resource.Resource = PendingOwners.back().GetReference();
		}
	}

	auto FVulkanPendingComputeState::PushConstants(
		FVulkanCommandListContext& InContext, EShaderStageFlags StageFlags,
		uint32 Offset, uint32 Size, const void* Data) -> void
	{
		check(CurrentPipelineState);
		checkf(StageFlags == EShaderStageFlags::Compute,
			"Compute push constants require compute stage visibility.");
		checkf(Data && Size > 0 && Offset <= std::numeric_limits<uint32>::max() - Size,
			"Compute push constants require one nonempty valid byte range.");
		const auto& Ranges = CurrentPipelineState->GetKey()
			.PipelineLayout.PushConstantRanges;
		checkf(std::ranges::any_of(Ranges,
			[&](const FPushConstantRange& Range) {
				return Range.StageFlags == EShaderStageFlags::Compute
					&& Offset >= Range.Offset
					&& Offset + Size <= Range.Offset + Range.Size;
			}), "Compute push constants are outside the active pipeline layout.");
		CurrentPipelineState->PushConstants(InContext, StageFlags, Offset, Size, Data);
	}

	auto FVulkanPendingComputeState::ClearDescriptorSetCache() -> void
	{
		if (!CachedDescriptorSets.empty())
		{
			auto StatsAccess = Device.AccessPipelineCacheStatistics();
			auto& Stats = StatsAccess.Get();
			check(Stats.DescriptorSnapshots.Occupancy > 0);
			--Stats.DescriptorSnapshots.Occupancy;
			check(Stats.DescriptorValueOccupancy >= CachedResources.size());
			Stats.DescriptorValueOccupancy -= CachedResources.size();
		}
		CachedResources.clear();
		CachedOwners.clear();
		CachedDescriptorSets.clear();
	}

	auto FVulkanPendingComputeState::NotifyDeletedPipeline(
		FVulkanComputePipelineState* PipelineState) -> void
	{
		if (CurrentPipelineState == PipelineState)
		{
			CurrentPipelineState = nullptr;
			PendingResources.clear();
			DeferredBindings.Clear();
			PendingOwners.clear();
			ClearDescriptorSetCache();
		}
	}

	auto FVulkanPendingComputeState::PrepareDescriptors(
		FVulkanCommandListContext& InContext) -> void
	{
		check(CurrentPipelineState);
		const auto Resolved = DeferredBindings.Resolve(Device, InContext, ERHIPipeline::Compute);
		if (!Resolved.empty()) SetShaderParameters(nullptr, Resolved, true);
		const uint64 Generation = Device.GetGlobalDescriptorPool().GetGeneration();
		if (DescriptorPoolGeneration != Generation)
		{
			ClearDescriptorSetCache();
			DescriptorPoolGeneration = Generation;
		}
		std::ranges::sort(PendingResources,
			[](const FRHIShaderParameterResource& A,
				const FRHIShaderParameterResource& B) {
				return std::tie(A.SetIndex, A.BindingIndex, A.ArrayElement)
					< std::tie(B.SetIndex, B.BindingIndex, B.ArrayElement);
			});
#if DO_CHECK
		const auto ValidationResult = ValidateShaderBindingCompleteness(
			CurrentPipelineState->GetKey().PipelineLayout, PendingResources);
		checkf(ValidationResult,
			"Invalid compute shader binding snapshot: {}", ToString(ValidationResult.error()));
#endif
		const auto ResourcesEqual = [](const auto& A, const auto& B) {
			if (A.size() != B.size()) return false;
			for (size_t Index = 0; Index < A.size(); ++Index)
			{
				if (A[Index].Resource != B[Index].Resource
					|| A[Index].SetIndex != B[Index].SetIndex
					|| A[Index].BindingIndex != B[Index].BindingIndex
					|| A[Index].ArrayElement != B[Index].ArrayElement
					|| A[Index].Type != B[Index].Type
					|| A[Index].Offset != B[Index].Offset
					|| A[Index].Size != B[Index].Size) return false;
			}
			return true;
		};
		std::vector<uint32> DynamicOffsets;
		for (const FRHIShaderParameterResource& Resource : PendingResources)
		{
			if (Resource.Type == ERHIBindingType::UniformBuffer
				|| Resource.Type == ERHIBindingType::UniformBufferDynamic
				|| Resource.Type == ERHIBindingType::StorageBuffer)
			{
				checkf(Resource.Resource->GetResourceType() == ERHIResourceType::BufferView,
					"Compute buffer descriptor requires a canonical buffer view.");
				const auto* View = FVulkanBufferView::Cast(Resource.Resource);
				const auto* Buffer = FVulkanBuffer::Cast(View->GetBuffer());
				const ERHIAccess Expected = Resource.Type == ERHIBindingType::StorageBuffer
					? (Buffer->IsDeferredReadOnly() ? ERHIAccess::ComputeShaderRead : ERHIAccess::ComputeShaderReadWrite)
					: ERHIAccess::ComputeUniformRead;
				ERHIAccess Tracked = ERHIAccess::None;
				checkf(Buffer->GetStateTracker().Validate(View->GetDesc().Offset,
					View->GetDesc().Size, Expected, Tracked),
					"Compute buffer descriptor binding state mismatch.");
				if (Resource.Type == ERHIBindingType::UniformBufferDynamic)
					DynamicOffsets.push_back(Resource.Offset);
			}
			else if (Resource.Type == ERHIBindingType::Texture
				|| Resource.Type == ERHIBindingType::StorageImage)
			{
				checkf(Resource.Resource->GetResourceType() == ERHIResourceType::TextureView,
					"Compute image descriptor requires a canonical texture view.");
				const auto* View = static_cast<const FVulkanTextureView*>(Resource.Resource);
				const auto* Texture = static_cast<const FVulkanTexture*>(View->GetTexture());
				const ERHIAccess Expected = Resource.Type == ERHIBindingType::Texture
					? ERHIAccess::ComputeShaderRead : ERHIAccess::ComputeShaderReadWrite;
				ERHIAccess Tracked = ERHIAccess::None;
				checkf(Texture->GetStateTracker().Validate(
					View->GetDesc().Range, Expected, Tracked),
					"Compute image descriptor binding state mismatch.");
			}
		}

		auto StatsAccess = Device.AccessPipelineCacheStatistics();
		auto& Stats = StatsAccess.Get();
		if (ResourcesEqual(CachedResources, PendingResources)
			&& !CachedDescriptorSets.empty())
		{
			++Stats.DescriptorSnapshots.Hits;
		}
		else
		{
			ClearDescriptorSetCache();
			++Stats.DescriptorSnapshots.Misses;
			const FVulkanDescriptorSetsLayout& Layout =
				CurrentPipelineState->GetDescriptorSetsLayout();
			const auto& Handles = Layout.GetLayoutHandles();
			if (!Handles.empty())
			{
				CachedDescriptorSets = Device.GetGlobalDescriptorPool().AllocateDescriptorSets(
					Handles, Layout.GetInfo().GetDescriptorRequirements());
				UpdateDescriptorSets(Device, PendingResources, CachedDescriptorSets);
				CachedResources = PendingResources;
				CachedOwners.clear();
				for (FRHIShaderParameterResource& Resource : CachedResources)
				{
					CachedOwners.emplace_back(Resource.Resource);
					Resource.Resource = CachedOwners.back().GetReference();
				}
				++Stats.DescriptorSnapshots.NativeCreations;
				++Stats.DescriptorAllocations;
				++Stats.DescriptorSnapshots.Occupancy;
				Stats.DescriptorValueOccupancy += CachedResources.size();
			}
		}
		if (!CachedDescriptorSets.empty())
		{
			InContext.GetCommandBuffer();
			InContext.RetainAllocation(Device.GetGlobalDescriptorPool().GetAllocationOwner());
			InContext.GetCommandBuffer()->GetHandle().bindDescriptorSets(
				vk::PipelineBindPoint::eCompute,
				CurrentPipelineState->GetPipelineLayout(), 0,
				CachedDescriptorSets, DynamicOffsets);
		}
	}

	auto FVulkanPendingComputeState::Dispatch(FVulkanCommandListContext& InContext,
		uint32 GroupCountX, uint32 GroupCountY, uint32 GroupCountZ) -> void
	{
		PrepareDescriptors(InContext);
		InContext.GetCommandBuffer()->GetHandle().dispatch(
			GroupCountX, GroupCountY, GroupCountZ);
	}

	FVulkanPendingGraphicsState::FVulkanPendingGraphicsState(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		const char* Validation = std::getenv("DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION");
		bFullDescriptorValidation = Validation && std::string_view(Validation) == "on";
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

	auto FVulkanPendingGraphicsState::SetDepthBias(float ConstantFactor,
		float Clamp, float SlopeFactor) -> void
	{
		DepthBiasConstantFactor = ConstantFactor;
		DepthBiasClamp = Clamp;
		DepthBiasSlopeFactor = SlopeFactor;
	}

	auto FVulkanPendingGraphicsState::SetShaderParameters(FRHIShader* InShader, const std::span<const FRHIShaderParameterResource>& InResourceParameters) -> void
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
#if DO_CHECK
		const auto ValidationResult = ValidateShaderParameterUpdate(Key.PipelineLayout, ShaderStage,
			InResourceParameters);
		checkf(ValidationResult,
			"Invalid shader parameter update: {}", ToString(ValidationResult.error()));
#endif
		CurrentDescriptorState->SetShaderParameters(InShader, InResourceParameters);
	}

	auto FVulkanGraphicsPipelineDescriptorState::SetShaderParameters(FRHIShader* InShader, const std::span<const FRHIShaderParameterResource>& InResourceParameters, bool bResolvingDeferred) -> void
	{
		if (!bResolvingDeferred) DeferredBindings.Update(InResourceParameters);
		for (const auto& ResourceParameter : InResourceParameters)
		{
			if (PendingSets.size() <= ResourceParameter.SetIndex)
				PendingSets.resize(static_cast<size_t>(ResourceParameter.SetIndex) + 1);
			auto& Set = PendingSets[ResourceParameter.SetIndex];
			const auto FoundIt = std::ranges::find_if(PendingShaderResources, [&ResourceParameter](const FRHIShaderParameterResource& ExistingParameter) {
				return ExistingParameter.SetIndex == ResourceParameter.SetIndex
					&& ExistingParameter.BindingIndex == ResourceParameter.BindingIndex
					&& ExistingParameter.ArrayElement == ResourceParameter.ArrayElement;
			});

			if (FoundIt == PendingShaderResources.end())
			{
				PendingShaderResources.push_back(ResourceParameter);
				bPendingResourcesSorted = false;
				bStructureValidated = false;
				Set.Selected.reset();
				Set.bOwnersDirty = true;
			}
			else
			{
				Set.bOwnersDirty |= FoundIt->Resource != ResourceParameter.Resource;
				if (FoundIt->Resource != ResourceParameter.Resource || FoundIt->Type != ResourceParameter.Type
					|| FoundIt->Size != ResourceParameter.Size
					|| (ResourceParameter.Type != ERHIBindingType::UniformBufferDynamic && FoundIt->Offset != ResourceParameter.Offset))
				{
					bStructureValidated = false;
					Set.Selected.reset();
				}
				*FoundIt = ResourceParameter;
			}
		}
		for (uint32 SetIndex = 0; SetIndex < PendingSets.size(); ++SetIndex)
		{
			auto& Set = PendingSets[SetIndex];
			if (!Set.bOwnersDirty) continue;
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			GVulkanDescriptorOwnerRebuildCount.fetch_add(1, std::memory_order_relaxed);
#endif
			std::vector<TRefCountPtr<FRHIResource>> NewOwners;
			for (const auto& Resource : PendingShaderResources)
				if (Resource.SetIndex == SetIndex) NewOwners.emplace_back(Resource.Resource);
			Set.ResourceOwners = std::move(NewOwners);
			Set.bOwnersDirty = false;
		}
	}

	auto FVulkanGraphicsPipelineDescriptorState::ResolveDeferredBuffers(
		FVulkanDevice& Device, FVulkanCommandListContext& Context) -> void
	{
		const auto Resolved = DeferredBindings.Resolve(Device, Context, ERHIPipeline::Graphics);
		if (!Resolved.empty()) SetShaderParameters(nullptr, Resolved, true);
	}

	auto FVulkanPendingGraphicsState::PrepareForDraw(FVulkanCommandListContext& InContext) -> void
	{
		check(CurrentPipelineState);
		check(CurrentDescriptorState);
		CurrentDescriptorState->ResolveDeferredBuffers(Device, InContext);
		const uint64 Generation = Device.GetGlobalDescriptorPool().GetGeneration();
		if (DescriptorPoolGeneration != Generation)
		{
			ClearDescriptorSetCache();
			DescriptorPoolGeneration = Generation;
		}

		FVulkanCommandBuffer* CmdBuffer = InContext.GetCommandBuffer();
		CmdBuffer->GetHandle().setViewport(0, Viewport);
		CmdBuffer->GetHandle().setScissor(0, Scissor);
		CmdBuffer->GetHandle().setDepthBias(DepthBiasConstantFactor,
			DepthBiasClamp, DepthBiasSlopeFactor);

		FVulkanGraphicsPipelineDescriptorState::FDescriptorSetsForDraw DescriptorSetsForDraw = CurrentDescriptorState->GetOrCreateDescriptorSetsForDraw(Device, *CurrentPipelineState);
		if (DescriptorSetsForDraw.DescriptorSets && !DescriptorSetsForDraw.DescriptorSets->empty())
		{
			InContext.RetainAllocation(Device.GetGlobalDescriptorPool().GetAllocationOwner());
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
		RemoveDescriptorCacheOccupancy(DescriptorEntryOccupancy, DescriptorValueOccupancy);
		DescriptorSetCache.clear();
		DescriptorSetCacheIndex.clear();
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
			PipelineStates.erase(It);
			VerifyDescriptorCacheOccupancy();
		}
	}

	auto FVulkanPendingGraphicsState::Reset() -> void
	{
		CurrentPipelineState = nullptr;
		CurrentDescriptorState = nullptr;
		PipelineStates.clear();
		ClearDescriptorSetCache();
	}

	auto FVulkanGraphicsPipelineDescriptorState::Reset() -> void
	{
		PendingShaderResources.clear();
		DeferredBindings.Clear();
		bPendingResourcesSorted = true;
		bStructureValidated = false;
		DrawValidationResourceIndices.clear();
		PendingSets.clear();
		ResolvedDescriptorSets.clear();
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

		const auto [It, bInserted] = PipelineStates.emplace(
			&InPipelineState, std::make_unique<FVulkanGraphicsPipelineDescriptorState>(*this));
		return *It->second;
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

	auto FVulkanGraphicsPipelineDescriptorState::GetOrCreateDescriptorSetsForDraw(FVulkanDevice& Device, FVulkanGraphicsPipelineState& PipelineState) -> FDescriptorSetsForDraw
	{
		const FVulkanDescriptorSetsLayout& DescriptorSetsLayout = PipelineState.GetDescriptorSetsLayout();
		const std::vector<vk::DescriptorSetLayout>& LayoutHandles = DescriptorSetsLayout.GetLayoutHandles();
		if (LayoutHandles.empty())
		{
			return {};
		}

		if (!bPendingResourcesSorted)
		{
			SortDescriptorResources(PendingShaderResources);
			bPendingResourcesSorted = true;
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			GVulkanDescriptorSortCount.fetch_add(1, std::memory_order_relaxed);
#endif
		}
		if (!bStructureValidated || Owner.bFullDescriptorValidation)
		{
			PendingSets.resize(LayoutHandles.size());
			size_t NextResource = 0;
			for (uint32 SetIndex = 0; SetIndex < PendingSets.size(); ++SetIndex)
			{
				auto& Set = PendingSets[SetIndex];
				Set.FirstResource = NextResource;
				while (NextResource < PendingShaderResources.size()
					&& PendingShaderResources[NextResource].SetIndex == SetIndex) ++NextResource;
				Set.ResourceCount = NextResource - Set.FirstResource;
			}
			DrawValidationResourceIndices.clear();
			uint64 BindingValidationVisits = 0;
			const auto CompletenessResult = RHIShaderParameterValidationInternal::VisitOrderedBindings(
				PipelineState.GetKey().PipelineLayout, PendingShaderResources,
				[&](const RHIShaderParameterValidationInternal::FBindingElement& Element,
					const FRHIShaderParameterResource& ResourceRecord) {
						const FBindingLayoutItem& Binding = *Element.Binding;
						const FRHIResource* Resource = ResourceRecord.Resource;
						if (Binding.Type == ERHIBindingType::UniformBufferDynamic
							|| Binding.Type == ERHIBindingType::Texture || Binding.Type == ERHIBindingType::StorageImage)
							DrawValidationResourceIndices.push_back(static_cast<size_t>(&ResourceRecord - PendingShaderResources.data()));
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
						}
						else
							checkf(Resource->GetResourceType() == ERHIResourceType::Sampler,
								"Sampler descriptor requires a sampler resource.");
					}, &BindingValidationVisits);
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			GVulkanBindingValidationVisitCount.fetch_add(
				BindingValidationVisits, std::memory_order_relaxed);
#endif
			checkf(CompletenessResult, "Invalid shader binding snapshot: {}", ToString(CompletenessResult.error()));
			bStructureValidated = true;
		}
		std::vector<uint32> DynamicOffsets;
		DynamicOffsets.reserve(DrawValidationResourceIndices.size());
		for (const size_t Index : DrawValidationResourceIndices)
		{
			const auto& Record = PendingShaderResources[Index];
			if (Record.Type == ERHIBindingType::UniformBufferDynamic)
			{
				const auto* View = static_cast<const FRHIBufferView*>(Record.Resource);
				const uint64 Alignment = Device.GetGpuProperties().limits.minUniformBufferOffsetAlignment;
				checkf(Alignment == 0 || (Record.Offset % Alignment) == 0,
					"Dynamic uniform offset is not device-aligned.");
				const uint64 BufferSize = View->GetBuffer()->GetSize();
				const uint64 Offset = View->GetDesc().Offset + static_cast<uint64>(Record.Offset);
				checkf(Offset <= BufferSize && View->GetDesc().Size <= BufferSize - Offset,
					"Dynamic uniform descriptor range exceeds its buffer.");
				DynamicOffsets.push_back(Record.Offset);
			}
			else
			{
				const auto* View = static_cast<const FRHITextureView*>(Record.Resource);
				const auto* Texture = static_cast<const FVulkanTexture*>(View->GetTexture());
				const ERHIAccess ExpectedAccess = Record.Type == ERHIBindingType::Texture
					? ERHIAccess::GraphicsShaderRead : ERHIAccess::GraphicsShaderReadWrite;
				ERHIAccess TrackedAccess = ERHIAccess::None;
				checkf(ValidateVulkanTextureDescriptorState(Texture->GetStateTracker(), View->GetDesc().Range,
					Record.Type, TrackedAccess),
					"Image descriptor binding state mismatch: texture='{}', set={}, binding={}, element={}, type={}, expectedAccess={}, trackedAccess={}.",
					Texture->GetDebugName(), Record.SetIndex, Record.BindingIndex, Record.ArrayElement,
					static_cast<uint32>(Record.Type), static_cast<uint32>(ExpectedAccess), static_cast<uint32>(TrackedAccess));
			}
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			GVulkanDescriptorDrawValidationVisitCount.fetch_add(1, std::memory_order_relaxed);
#endif
		}
		ResolvedDescriptorSets.resize(LayoutHandles.size());
		for (uint32 SetIndex = 0; SetIndex < PendingSets.size(); ++SetIndex)
		{
			auto& Set = PendingSets[SetIndex];
			auto Entry = Set.Selected.lock();
			if (Entry)
			{
				++Device.AccessPipelineCacheStatistics().Get().DescriptorSnapshots.Hits;
				Owner.TouchDescriptorCacheEntry(*Entry);
			}
			else
			{
				FVulkanDescriptorRequirements Requirements;
				Requirements.MaxSets = 1;
				for (const auto& Binding : DescriptorSetsLayout.GetInfo().GetLayouts()[SetIndex].LayoutBindings)
					Requirements.DescriptorCounts[Binding.descriptorType] += Binding.descriptorCount;
				Entry = Owner.ResolveDescriptorSet(LayoutHandles[SetIndex],
					std::span<const FRHIShaderParameterResource>(PendingShaderResources).subspan(Set.FirstResource, Set.ResourceCount),
					Requirements, SetIndex);
				Set.Selected = Entry;
			}
			ResolvedDescriptorSets[SetIndex] = Entry->DescriptorSet;
		}
		return {&ResolvedDescriptorSets, std::move(DynamicOffsets)};
	}

	auto FVulkanPendingGraphicsState::ResolveDescriptorSet(vk::DescriptorSetLayout Layout,
		std::span<const FRHIShaderParameterResource> Resources,
		const FVulkanDescriptorRequirements& Requirements, uint32 SetIndex) -> std::shared_ptr<FDescriptorEntry>
	{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
		GVulkanDescriptorHashCount.fetch_add(1, std::memory_order_relaxed);
#endif
		FXxHash64Builder HashBuilder;
		HashBuilder.UpdateValue(Layout);
		for (const auto& Resource : Resources)
		{
			HashBuilder.UpdateValue(Resource.BindingIndex);
			HashBuilder.UpdateValue(Resource.ArrayElement);
			HashBuilder.UpdateValue(Resource.Type);
			HashBuilder.UpdateValue(reinterpret_cast<uintptr_t>(Resource.Resource));
			HashBuilder.UpdateValue(Resource.Size);
			if (Resource.Type != ERHIBindingType::UniformBufferDynamic) HashBuilder.UpdateValue(Resource.Offset);
		}
		const uint64 Hash = HashBuilder.Finalize().HashValue;
		const auto [First, Last] = DescriptorSetCacheIndex.equal_range(Hash);
		for (auto Candidate = First; Candidate != Last; ++Candidate)
		{
			auto& Entry = DescriptorSetCache[Candidate->second];
			// The device layout cache interns complete structural layouts. Set index
			// is external to compatibility; every draw explicitly binds all sets.
			if (Entry->Layout == Layout && std::ranges::equal(Entry->Resources, Resources,
				[](const auto& A, const auto& B) {
					return A.Resource == B.Resource && A.BindingIndex == B.BindingIndex
						&& A.ArrayElement == B.ArrayElement && A.Type == B.Type && A.Size == B.Size
						&& (A.Type == ERHIBindingType::UniformBufferDynamic || A.Offset == B.Offset);
				}))
			{
				++Device.AccessPipelineCacheStatistics().Get().DescriptorSnapshots.Hits;
				TouchDescriptorCacheEntry(*Entry);
				return Entry;
			}
		}
		++Device.AccessPipelineCacheStatistics().Get().DescriptorSnapshots.Misses;
		auto Entry = std::make_shared<FDescriptorEntry>();
		Entry->Hash = Hash;
		Entry->Layout = Layout;
		Entry->Resources.assign(Resources.begin(), Resources.end());
		Entry->ResourceOwners.reserve(Resources.size());
		for (const auto& Resource : Resources) Entry->ResourceOwners.emplace_back(Resource.Resource);
		Entry->DescriptorSet = Device.GetGlobalDescriptorPool().AllocateDescriptorSets(
			std::span<const vk::DescriptorSetLayout>(&Layout, 1), Requirements).front();
		UpdateDescriptorSets(Device, Resources, std::span<const vk::DescriptorSet>(&Entry->DescriptorSet, 1), SetIndex);
		++Device.AccessPipelineCacheStatistics().Get().DescriptorSnapshots.NativeCreations;
		++Device.AccessPipelineCacheStatistics().Get().DescriptorAllocations;
		DescriptorSetCache.push_back(Entry);
		DescriptorSetCacheIndex.emplace(Hash, DescriptorSetCache.size() - 1);
		AddDescriptorCacheOccupancy(1, Resources.size());
		TouchDescriptorCacheEntry(*Entry);
		EnforceDescriptorCacheBudget();
		return Entry;
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
		auto StatsAccess = Device.AccessPipelineCacheStatistics();
		auto& Stats = StatsAccess.Get();
		check(Stats.DescriptorSnapshots.Occupancy
			<= std::numeric_limits<uint64>::max() - EntryCount);
		check(Stats.DescriptorValueOccupancy
			<= std::numeric_limits<uint64>::max() - ValueCount);
		Stats.DescriptorSnapshots.Occupancy += EntryCount;
		Stats.DescriptorValueOccupancy += ValueCount;
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
		auto StatsAccess = Device.AccessPipelineCacheStatistics();
		auto& Stats = StatsAccess.Get();
		check(EntryCount <= Stats.DescriptorSnapshots.Occupancy);
		check(ValueCount <= Stats.DescriptorValueOccupancy);
		Stats.DescriptorSnapshots.Occupancy -= EntryCount;
		Stats.DescriptorValueOccupancy -= ValueCount;
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
		for (const auto& Entry : DescriptorSetCache)
		{
			GVulkanDescriptorOccupancyVerificationVisitCount.fetch_add(1, std::memory_order_relaxed);
			++EntryCount;
			ValueCount += Entry->Resources.size();
		}
		checkfSlow(EntryCount == DescriptorEntryOccupancy
			&& ValueCount == DescriptorValueOccupancy,
			"Incremental descriptor occupancy diverged from owned cache state.");
#endif
	}

	auto FVulkanPendingGraphicsState::EnforceDescriptorCacheBudget() -> void
	{
		while (DescriptorEntryOccupancy > 512 || DescriptorValueOccupancy > 8192)
		{
			const auto Victim = std::ranges::min_element(DescriptorSetCache, {},
				[](const auto& Entry) { return Entry->LastUsed; });
			check(Victim != DescriptorSetCache.end());
			RemoveDescriptorCacheOccupancy(1, (*Victim)->Resources.size());
			DescriptorSetCache.erase(Victim);
			RebuildDescriptorCacheIndex();
			++Device.AccessPipelineCacheStatistics().Get().DescriptorSnapshots.Evictions;
		}
	}

	auto FVulkanPendingGraphicsState::RebuildDescriptorCacheIndex() -> void
	{
		DescriptorSetCacheIndex.clear();
		for (size_t Index = 0; Index < DescriptorSetCache.size(); ++Index)
			DescriptorSetCacheIndex.emplace(DescriptorSetCache[Index]->Hash, Index);
	}
} // namespace Durin::VulkanRHI
