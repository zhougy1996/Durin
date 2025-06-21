#include "MonaGlobals.h"

#include "RHI.h"
#include "Application/MonaApplication.h"

ImGuiContext* GImGuiContext = nullptr;

static auto ImGuiInit() -> void
{
	check(GDynamicRHI);
	GImGuiContext = ImGui::CreateContext();
	ImGui::SetCurrentContext(GImGuiContext);
}

auto MonaInit() -> void
{
	ImGuiInit();
	FMonaApplication::Create();
	FMonaApplication::Get().Initialize();
}
