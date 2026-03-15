#include "DynamicRHI.h"

namespace Doge
{
	FDynamicRHI* GDynamicRHI = nullptr;

	auto FDynamicRHI::RHIEndFrame_RenderThread(FRHICommandListImmediate& RHICmdList) -> void
	{
	}
}
