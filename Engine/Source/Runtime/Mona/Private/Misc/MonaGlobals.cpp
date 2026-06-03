#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"

namespace Durin::Mona
{
	auto MonaInit() -> void
	{
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();

		FModuleManager::Get().LoadModule("MonaImGuiBackend");

		DURIN_DEBUG(STR("Mona initialized successfully."));
	}

	auto MonaShutdown() -> void
	{
		FModuleManager::Get().UnloadModule("MonaImGuiBackend");

		FMonaApplication::Shutdown();
		DURIN_DEBUG(STR("Mona shutdown."));
	}

	auto NewFrame() -> void
	{
		if (GMonaUIBackend)
		{
			GMonaUIBackend->NewFrame();
		}
	}

	auto Render() -> void
	{
		if (GMonaUIBackend)
		{
			GMonaUIBackend->Render();
		}
	}
} // namespace Durin::Mona
