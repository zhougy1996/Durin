#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "IMonaUIBackend.h"

#if DURIN_WITH_DEVELOPER_TOOLS
	#include "MonaImGuiBackend.h"
#endif

namespace Durin::Mona
{
	namespace
	{
		std::unique_ptr<IMonaUIBackend> GUIBackend;

		auto InstallDefaultUIBackend() -> void
		{
#if DURIN_WITH_DEVELOPER_TOOLS
			if (!GUIBackend)
			{
				GUIBackend = std::make_unique<FMonaImGuiUIBackend>();
			}
#endif
		}
	}

	auto MonaInit() -> void
	{
		FMonaApplication::Create();
		FMonaApplication::Get().Initialize();
		InstallDefaultUIBackend();
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
#if DURIN_WITH_DEVELOPER_TOOLS && !DURIN_WITH_EDITOR
		FMonaImGuiBackend::ShowDemoWindow();
#endif
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

} // namespace Durin::Mona
