#include "HAL/NativePresentationTarget.h"

#if defined(__APPLE__)
#include "MacOS/MacOSPresentationTarget.h"
#endif

namespace Durin
{
	FNativePresentationTarget::FNativePresentationTarget(void* InWindow) : Window(InWindow) {}
	FNativePresentationTarget::~FNativePresentationTarget() = default;

#if defined(__APPLE__)
	FMacOSPresentationTarget::FMacOSPresentationTarget(void* Window, CA::MetalLayer* InLayer)
		: FNativePresentationTarget(Window), Layer(InLayer) {}
	FMacOSPresentationTarget::~FMacOSPresentationTarget() = default;
#endif
}
