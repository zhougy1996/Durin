#include "MonaBackendGlobals.h"

#include "RHI.h"
#include "Application/MonaApplication.h"
#include "MonaImGuiEventHandler.h"


namespace Doge::Mona
{
	static ImGuiContext* GImGuiContext = nullptr;

	static auto ImGuiInit() -> void
	{
		check(GDynamicRHI);
		GImGuiContext = ImGui::CreateContext();
		ImGui::SetCurrentContext(GImGuiContext);
	}

	auto BackendInit() -> void
	{
		ImGuiInit();
	}

	auto BackendClose() -> void
	{
	}

	auto InitMonaBackendEventHandler() -> void
	{
		check(FMonaApplication::IsInitialized());
		auto& App = FMonaApplication::Get();
		App.SetMonaEventHandler(std::make_unique<FMonaImGuiEventHandler>());
	}

} // namespace Doge::Mona