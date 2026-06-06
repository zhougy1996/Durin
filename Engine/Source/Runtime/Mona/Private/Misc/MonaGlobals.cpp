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

	auto RenderWindowRefresh(void* NativeWindowHandle) -> bool
	{
		if (GActiveUIBackend)
		{
			return GActiveUIBackend->RenderWindowRefresh(NativeWindowHandle);
		}

		return false;
	}
} // namespace Durin::Mona
