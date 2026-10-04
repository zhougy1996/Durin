#if !defined(__APPLE__)

#include "DynamicRHI.h"

namespace Durin
{
	class FMetalUnsupportedModule final : public IDynamicRHIModule
	{
	public:
		auto CreateRHI() -> FDynamicRHI* override { return nullptr; }
	};

	IMPLEMENT_MODULE(FMetalUnsupportedModule, MetalRHI)
}

#endif
