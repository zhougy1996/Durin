#include "UniformBuffer.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	auto FUniformBuffer::Create() -> std::shared_ptr<FUniformBuffer>
	{
		return CreateRenderResource<FUniformBuffer>();
	}
	auto FUniformBuffer::Update(FRHICommandListBase& Commands, FByteView Values,
		std::span<FRHIResource* const> Resources, std::span<const FRHIUniformBufferResource> Members) -> bool
	{
		CheckRenderingThread();
		if (!IsInitialized()) InitResource(Commands);
		const FRHIUniformBufferLayout Layout{static_cast<uint32>(Values.size()), {Members.begin(), Members.end()}};
		if (Buffer && Buffer->GetLayout() == Layout && std::ranges::equal(Data, Values)
			&& std::ranges::equal(Buffer->GetResourceTable(), Resources, [](const auto& A, auto* B) { return A.GetReference() == B; })) return true;
		if (!Buffer || Buffer->GetLayout() != Layout)
		{
			auto Replacement = Commands.CreateUniformBuffer(Layout, ERHIBufferLifetimeUsage::MultiFrame, Values, Resources);
			if (!Replacement) return false;
			Buffer = std::move(Replacement);
		}
		else Commands.UpdateUniformBuffer(Buffer, Values, Resources);
		if (!Buffer) return false;
		Data.assign(Values.begin(), Values.end());
		return true;
	}
	auto FUniformBuffer::ReleaseRHIForAllResources() -> void
	{
		std::vector<FUniformBuffer*> Buffers;
		VisitInitializedRenderResources([&](FRenderResource& Resource) {
			if (auto* Uniform = dynamic_cast<FUniformBuffer*>(&Resource)) Buffers.push_back(Uniform);
		});
		for (auto* Buffer : Buffers) Buffer->ReleaseResource();
	}
}
