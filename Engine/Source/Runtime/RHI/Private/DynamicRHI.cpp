#include "DynamicRHI.h"

#include "RHICommandList.h"

namespace Doge
{
	FDynamicRHI* GDynamicRHI = nullptr;

	auto FDynamicRHI::RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void
	{
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread, ERHISubmitFlags::EndFrame);
	}
}
