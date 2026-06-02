#include "MonaGlobals.h"

#include "Application/MonaApplication.h"

namespace Durin::Mona
{
	auto MonaInit() -> void
	{
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();
		DURIN_DEBUG(STR("Mona initialized successfully."));
	}

	auto MonaShutdown() -> void
	{
		FMonaApplication::Shutdown();
		DURIN_DEBUG(STR("Mona shutdown."));
	}

	auto NewFrame() -> void
	{
	}

	auto Render() -> void
	{
	}
} // namespace Durin::Mona
