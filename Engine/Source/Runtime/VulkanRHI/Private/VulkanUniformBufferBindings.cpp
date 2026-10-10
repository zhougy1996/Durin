#include "VulkanUniformBufferBindings.h"
#include "VulkanContext.h"
#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"
#include "VulkanView.h"

namespace Durin::VulkanRHI
{
	auto FVulkanUniformBufferBindings::Update(
		std::span<const FRHIShaderParameterResource> Parameters) -> void
	{
		for (const auto& Parameter : Parameters)
		{
			const auto It = std::ranges::find_if(Bindings, [&](const FBinding& Binding) {
				return Binding.Parameter.SetIndex == Parameter.SetIndex
					&& Binding.Parameter.BindingIndex == Parameter.BindingIndex
					&& Binding.Parameter.ArrayElement == Parameter.ArrayElement;
			});
			if (IsUniformBufferResource(Parameter.Resource))
			{
				if (It != Bindings.end() && It->Uniform.GetReference() == Parameter.Resource)
				{
					It->Parameter = Parameter;
					It->bDirty = true;
					continue;
				}
				FBinding Binding{Parameter, static_cast<FRHIUniformBuffer*>(Parameter.Resource), {}};
				if (It == Bindings.end()) Bindings.push_back(std::move(Binding));
				else *It = std::move(Binding);
			}
			else if (It != Bindings.end()) Bindings.erase(It);
		}
	}

	auto FVulkanUniformBufferBindings::Resolve(FVulkanDevice& Device) -> std::vector<FRHIShaderParameterResource>
	{
		std::vector<FRHIShaderParameterResource> Result;
		Result.reserve(Bindings.size());
		for (auto& Binding : Bindings)
		{
			if (!Binding.bDirty) continue;
			const bool bDynamic = Binding.Parameter.Type == ERHIBindingType::UniformBufferDynamic;
			const auto& Allocation = static_cast<FVulkanUniformBuffer*>(Binding.Uniform.GetReference())->GetAllocation();
			TRefCountPtr<FVulkanBuffer> Buffer = Allocation->Buffer;
			const uint64 AllocationOffset = Allocation->Offset;
			FRHIBufferViewDesc ViewDesc{bDynamic ? 0 : Binding.Parameter.Offset,
				Binding.Parameter.Size, ERHIBufferViewType::Uniform};
			ViewDesc.Offset += AllocationOffset;
			const uint64 PhysicalOffset = ViewDesc.Offset;
			if (bDynamic) ViewDesc.Offset = 0;
			auto Resolved = Binding.Resolved;
			if (!Resolved || Resolved->Allocation != Allocation || Resolved->bDynamic != bDynamic
				|| Resolved->PhysicalOffset != PhysicalOffset || Resolved->View->GetDesc() != ViewDesc)
			{
				Resolved = std::make_shared<FResolved>();
				Resolved->Allocation = Allocation;
				Resolved->PhysicalOffset = PhysicalOffset;
				Resolved->bDynamic = bDynamic;
				Resolved->View = bDynamic
					? Device.GetRHI().RHIGetOrCreateBufferView(Buffer, ViewDesc)
					: TRefCountPtr<FRHIBufferView>(new FVulkanBufferView(Device, Buffer, ViewDesc));
				requiref(Resolved->View, "Could not create a shader buffer descriptor view.");
				Binding.Resolved = Resolved;
			}
			Binding.bDirty = false;
			auto Parameter = Binding.Parameter;
			Parameter.Resource = Resolved->View.GetReference();
			if (bDynamic)
			{
				const uint64 Offset = Resolved->PhysicalOffset + Parameter.Offset;
				requiref(Offset <= UINT32_MAX, "Dynamic uniform offset exceeds Vulkan limits.");
				Parameter.Offset = static_cast<uint32>(Offset);
			}
			Result.push_back(Parameter);
		}
		return Result;
	}

	auto FVulkanUniformBufferBindings::PrepareForUse(
		FVulkanCommandListContext& Context, ERHIPipeline Pipeline) -> void
	{
		for (const auto& Binding : Bindings)
		{
			const auto& Resolved = Binding.Resolved;
			check(Resolved && !Binding.bDirty);
			// A new submission must retain the captured allocation even without a rebind.
			auto* Buffer = FVulkanBuffer::Cast(Resolved->View->GetBuffer());
			// Read-to-read changes need no memory dependency for immutable host-initialized data.
			const uint64 Offset = Resolved->PhysicalOffset
				+ (Resolved->bDynamic ? Binding.Parameter.Offset : 0);
			const ERHIAccess Expected = Pipeline == ERHIPipeline::Compute
				? ERHIAccess::ComputeUniformRead : ERHIAccess::GraphicsUniformRead;
			ERHIAccess Tracked = ERHIAccess::None;
			if (!Buffer->GetStateTracker().Validate(Offset, Resolved->View->GetDesc().Size, Expected, Tracked))
				Buffer->GetStateTracker().Apply(Offset, Resolved->View->GetDesc().Size, Expected);
			Context.RetainAllocation(Resolved);
		}
	}
}
