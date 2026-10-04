#pragma once

#include "CoreMinimal.h"
#include "HAL/NativePresentationTarget.h"

namespace CA
{
	class MetalLayer;
}

namespace Durin
{
	// Native macOS boundary, consumed only by platform producers and RHI backends.
	// The window owns the borrowed layer; a backend acquires its own native reference.
	class CORE_API FMacOSPresentationTarget final : public FNativePresentationTarget
	{
	public:
		FMacOSPresentationTarget(void* Window, CA::MetalLayer* InLayer);
		~FMacOSPresentationTarget() override;
		auto GetMetalLayer() const -> CA::MetalLayer* { return Layer; }
	private:
		CA::MetalLayer* const Layer;
	};
}
