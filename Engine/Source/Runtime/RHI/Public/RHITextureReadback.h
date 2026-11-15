#pragma once

#include "CoreMinimal.h"
#include "RHIGPUReadback.h"

namespace Durin
{
	// Texture-only consuming read. Readiness describes copy completion, even after consumption.
	class FRHITextureReadback final : public FRHIGPUMemoryReadback
	{
	public:
		auto TakePixels(FByteBuffer& Out) -> bool { return TakeData(Out); }
	};
}
