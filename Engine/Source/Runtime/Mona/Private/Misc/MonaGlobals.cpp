#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaCoreGlobals.h"
#include "MonaUIInterface.h"

namespace Durin::Mona
{
	auto MonaInit() -> void
	{
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();

		FModuleManager::Get().LoadModule("MonaImGui");

		DURIN_DEBUG(STR("Mona initialized successfully."));
	}

	auto MonaShutdown() -> void
	{
		FModuleManager::Get().UnloadModule("MonaImGui");

		FMonaApplication::Shutdown();
		DURIN_DEBUG(STR("Mona shutdown."));
	}

	auto NewFrame() -> void
	{
		if (GMonaUI)
		{
			GMonaUI->NewFrame();
		}
	}

	auto Render() -> void
	{
		if (GMonaUI)
		{
			GMonaUI->Render();
		}
	}
} // namespace Durin::Mona
