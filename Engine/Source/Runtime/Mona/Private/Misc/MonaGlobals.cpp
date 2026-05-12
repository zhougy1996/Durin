#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaBackend.h"

namespace Doge::Mona
{
	auto MonaInit() -> void
	{
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();
		FMonaBackend::Initialize();
		DOGE_DEBUG(STR("Mona initialized successfully."));
	}

	auto MonaShutdown() -> void
	{
		FMonaBackend::Shutdown();
		FMonaApplication::Shutdown();
		DOGE_DEBUG(STR("Mona shutdown."));
	}

	auto MonaUI_NewFrame() -> void
	{
		FMonaBackend::NewFrame();
	}

	auto MonaUI_Render() -> void
	{
		FMonaBackend::Render();
	}
} // namespace Doge::Mona