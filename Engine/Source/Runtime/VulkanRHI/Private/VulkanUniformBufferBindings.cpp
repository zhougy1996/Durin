#include "VulkanUniformBufferBindings.h"
#include "VulkanContext.h"

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

	auto FVulkanUniformBufferBindings::Resolve() -> std::vector<FRHIShaderParameterResource>
	{
		std::vector<FRHIShaderParameterResource> Result;
		Result.reserve(Bindings.size());
		for (auto& Binding : Bindings)
		{
			if (!Binding.bDirty) continue;
			Binding.Allocation = static_cast<FVulkanUniformBuffer*>(Binding.Uniform.GetReference())->GetAllocation();
			Binding.bDirty = false;
			auto Parameter = Binding.Parameter;
			Parameter.Resource = Binding.Allocation->Buffer.GetReference();
			const uint64 Offset = Binding.Allocation->Offset + Parameter.Offset;
			requiref(Offset <= UINT32_MAX, "Uniform buffer offset exceeds shader parameter limits.");
			Parameter.Offset = static_cast<uint32>(Offset);
			Result.push_back(Parameter);
		}
		return Result;
	}

	auto FVulkanUniformBufferBindings::PrepareForUse(
		FVulkanCommandListContext& Context, ERHIPipeline Pipeline) -> void
	{
		for (const auto& Binding : Bindings)
		{
			const auto& Allocation = Binding.Allocation;
			check(Allocation && !Binding.bDirty);
			auto* Buffer = Allocation->Buffer.GetReference();
			const uint64 Offset = Allocation->Offset + Binding.Parameter.Offset;
			const ERHIAccess Expected = Pipeline == ERHIPipeline::Compute
				? ERHIAccess::ComputeUniformRead : ERHIAccess::GraphicsUniformRead;
			ERHIAccess Tracked = ERHIAccess::None;
			if (!Buffer->GetStateTracker().Validate(Offset, Binding.Parameter.Size, Expected, Tracked))
				Buffer->GetStateTracker().Apply(Offset, Binding.Parameter.Size, Expected);
			Context.RetainAllocation(Allocation);
		}
	}
}
