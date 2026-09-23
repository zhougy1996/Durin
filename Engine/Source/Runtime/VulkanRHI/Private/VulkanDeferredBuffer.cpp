#include "VulkanDeferredBuffer.h"
#include "VulkanBuffer.h"
#include "VulkanContext.h"
#include "VulkanDevice.h"
#include "VulkanView.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		struct FDeferredBacking { TRefCountPtr<FVulkanBuffer> Buffer; };
	}

	auto FVulkanDeferredBufferBindings::Update(
		std::span<const FRHIShaderParameterResource> Parameters) -> void
	{
		for (const auto& Parameter : Parameters)
		{
			const auto It = std::ranges::find_if(Bindings, [&](const FBinding& Binding) {
				return Binding.Parameter.SetIndex == Parameter.SetIndex
					&& Binding.Parameter.BindingIndex == Parameter.BindingIndex
					&& Binding.Parameter.ArrayElement == Parameter.ArrayElement;
			});
			if (IsCPUAuthoredBufferResource(Parameter.Resource))
			{
				if (It != Bindings.end() && It->Logical.GetReference() == Parameter.Resource)
				{
					It->Parameter = Parameter;
					continue;
				}
				FBinding Binding{Parameter, static_cast<FRHIBufferView*>(Parameter.Resource), {}};
				if (It == Bindings.end()) Bindings.push_back(std::move(Binding));
				else *It = std::move(Binding);
			}
			else if (It != Bindings.end()) Bindings.erase(It);
		}
	}

	auto FVulkanDeferredBufferBindings::Resolve(FVulkanDevice& Device,
		FVulkanCommandListContext& Context, ERHIPipeline Pipeline) -> std::vector<FRHIShaderParameterResource>
	{
		std::vector<FRHIShaderParameterResource> Result;
		Result.reserve(Bindings.size());
		for (auto& Binding : Bindings)
		{
			const auto Snapshot = FRHIDeferredBufferBackend::ResolveSnapshot(*Binding.Logical->GetBuffer());
			if (!Binding.Resolved || Binding.Resolved->Snapshot != Snapshot)
			{
				const auto& Desc = Binding.Logical->GetBuffer()->GetDesc();
				auto Backing = std::static_pointer_cast<FDeferredBacking>(
					FRHIDeferredBufferBackend::GetBacking(*Snapshot, &Context));
				if (!Backing)
				{
					Backing = std::make_shared<FDeferredBacking>();
					Backing->Buffer = new FVulkanBuffer(Device, FRHIBufferCreateDesc::Create(
						"DeferredBufferVersion", Desc.Size, Desc.Stride, Desc.Usage | EBufferUsageFlags::Dynamic));
					Backing->Buffer->InitializeDeferredReadOnly(Snapshot->GetData());
					FRHIDeferredBufferBackend::SetBacking(*Snapshot, &Context, Backing);
				}
				const auto& ViewDesc = Binding.Logical->GetDesc();
				const auto& Limits = Device.GetGpuProperties().limits;
				const bool bUniform = ViewDesc.Type == ERHIBufferViewType::Uniform;
				const uint64 Alignment = bUniform ? Limits.minUniformBufferOffsetAlignment
					: Limits.minStorageBufferOffsetAlignment;
				const uint64 MaxRange = bUniform ? Limits.maxUniformBufferRange : Limits.maxStorageBufferRange;
				if ((Alignment && ViewDesc.Offset % Alignment != 0) || ViewDesc.Size > MaxRange)
					throw std::runtime_error("Deferred buffer view exceeds native binding limits.");
				auto Resolved = std::make_shared<FResolved>();
				Resolved->Snapshot = Snapshot;
				Resolved->View = new FVulkanBufferView(Device, Backing->Buffer, ViewDesc);
				Binding.Resolved = std::move(Resolved);
			}
			// Each queue submission owns the exact version, even after a later update/rebind.
			auto* Buffer = FVulkanBuffer::Cast(Binding.Resolved->View->GetBuffer());
			const bool bUniform = Binding.Logical->GetDesc().Type == ERHIBufferViewType::Uniform;
			// Read-to-read changes need no memory dependency for immutable host-initialized data.
			Buffer->GetStateTracker().Apply(0, Buffer->GetSize(), Pipeline == ERHIPipeline::Compute
				? (bUniform ? ERHIAccess::ComputeUniformRead : ERHIAccess::ComputeShaderRead)
				: (bUniform ? ERHIAccess::GraphicsUniformRead : ERHIAccess::GraphicsShaderRead));
			Context.RetainAllocation(Binding.Resolved);
			auto Parameter = Binding.Parameter;
			Parameter.Resource = Binding.Resolved->View.GetReference();
			Result.push_back(Parameter);
		}
		return Result;
	}
}
