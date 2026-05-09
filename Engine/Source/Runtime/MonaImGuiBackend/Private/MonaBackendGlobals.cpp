#include "MonaBackendGlobals.h"

#include "RHI.h"
#include "Application/MonaApplication.h"
#include "MonaImGuiEventHandler.h"


namespace Doge::Mona
{
	ImGuiContext* GMonaImGuiContext = nullptr;

	static auto ImGuiInit() -> void
	{
		check(GDynamicRHI);
		GMonaImGuiContext = ImGui::CreateContext();
		ImGui::SetCurrentContext(GMonaImGuiContext);
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