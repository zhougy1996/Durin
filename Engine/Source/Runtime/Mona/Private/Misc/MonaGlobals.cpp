#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "Misc/ApplicationCoreGlobals.h"
#include "MonaBackend.h"
#include "MonaImGuiBackend.h"

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

	auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		FMonaImGuiBackend::BindMainViewportToWindow(Window);
	}

	auto ShowDemoWindow() -> void
	{
		FMonaBackend::ShowDemoWindow();
	}
} // namespace Durin::Mona
