#include "MonaBackendGlobals.h"

#include "RHI.h"

namespace Doge::Mona
{
	namespace ImGuiBackend
	{
		ImGuiContext* GImGuiContext = nullptr;

		auto ImGuiInit() -> void
		{
			check(GDynamicRHI);
			GImGuiContext = ImGui::CreateContext();
			ImGui::SetCurrentContext(GImGuiContext);
		}
	}

	auto BackendInit() -> void
	{
		ImGuiBackend::ImGuiInit();
	}
}