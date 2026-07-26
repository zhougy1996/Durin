#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaUIBackend.h"
#include "MonaCoreGlobals.h"

namespace Durin::Mona
{
	auto MonaInit() -> void
	{
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();

#if DURIN_WITH_EDITOR
		FModuleManager::Get().LoadModule("MonaImGui");
#endif

		DURIN_DEBUG(STR("Mona initialized successfully."));
	}

	auto MonaShutdown() -> void
	{
#if DURIN_WITH_EDITOR
		FModuleManager::Get().UnloadModule("MonaImGui");
#endif

		FMonaApplication::Shutdown();
		DURIN_DEBUG(STR("Mona shutdown."));
	}

	auto NewFrame() -> void
	{
		if (GActiveUIBackend)
		{
			GActiveUIBackend->NewFrame();
		}
	}

	auto Render() -> void
	{
		if (GActiveUIBackend)
		{
			GActiveUIBackend->Render();
		}
	}

} // namespace Durin::Mona
