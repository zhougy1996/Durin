#include "UniformBuffer.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	auto FUniformBuffer::Create() -> std::shared_ptr<FUniformBuffer>
	{
		return CreateRenderResource<FUniformBuffer>();
	}
	auto FUniformBuffer::Update(FRHICommandListBase& Commands, FByteView Values) -> bool
	{
		CheckRenderingThread();
		if (!IsInitialized()) InitResource(Commands);
		if (Buffer && std::ranges::equal(Data, Values)) return true;
		if (!Buffer || Data.size() != Values.size())
			Buffer = Commands.CreateUniformBuffer({static_cast<uint32>(Values.size())}, ERHIBufferLifetimeUsage::MultiFrame, Values);
		else Commands.UpdateUniformBuffer(Buffer, Values);
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
