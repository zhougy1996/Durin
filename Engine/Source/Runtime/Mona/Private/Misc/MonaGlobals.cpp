#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaBackend.h"

namespace Durin::Mona
{
	auto MonaInit() -> void
	{
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();
		FMonaBackend::Initialize();
		DURIN_DEBUG(STR("Mona initialized successfully."));
	}

	auto MonaShutdown() -> void
	{
		FMonaBackend::Shutdown();
		FMonaApplication::Shutdown();
		DURIN_DEBUG(STR("Mona shutdown."));
	}

	auto NewFrame() -> void
	{
		FMonaBackend::NewFrame();
	}

	auto Render() -> void
	{
		FMonaBackend::Render();
	}
} // namespace Doge::Mona