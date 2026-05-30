#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "IMonaUIBackend.h"

namespace Durin::Mona
{
	namespace
	{
		std::unique_ptr<IMonaUIBackend> GUIBackend;
	}

	auto MonaInit() -> void
	{
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();
		if (GUIBackend)
		{
			GUIBackend->Initialize();
		}
		DURIN_DEBUG(STR("Mona initialized successfully."));
	}

	auto MonaShutdown() -> void
	{
		if (GUIBackend)
		{
			GUIBackend->Shutdown();
			GUIBackend.reset();
		}
		FMonaApplication::Shutdown();
		DURIN_DEBUG(STR("Mona shutdown."));
	}

	auto NewFrame() -> void
	{
		if (GUIBackend)
		{
			GUIBackend->NewFrame();
		}
	}

	auto Render() -> void
	{
		if (GUIBackend)
		{
			GUIBackend->Render();
		}
	}

	auto SetUIBackend(std::unique_ptr<IMonaUIBackend> InBackend) -> void
	{
		GUIBackend = std::move(InBackend);
	}

	auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		if (GUIBackend)
		{
			GUIBackend->BindMainViewportToWindow(Window);
		}
	}

	auto ShowDemoWindow() -> void
	{
		if (GUIBackend)
		{
			GUIBackend->ShowDemoWindow();
		}
	}
} // namespace Durin::Mona
