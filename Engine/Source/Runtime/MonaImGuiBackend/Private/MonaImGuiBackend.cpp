#include "MonaImGuiBackend.h"

#include "Application/MonaApplication.h"
#include "MonaImGuiEventHandler.h"
#include "ImGuiRHIImpl.h"

#include "DynamicRHI.h"

namespace Doge::Mona
{
	auto FMonaImGuiBackend::Initialize() -> void
	{
		check(GDynamicRHI);
		MonaImGuiBackend::GMonaImGuiContext = ImGui::CreateContext();
		ImGui::SetCurrentContext(MonaImGuiBackend::GMonaImGuiContext);

		MonaImGuiBackend::ImGuiRHIImpl_Init();;

		// Set the Mona event handler to the application. This will allow us to receive input events and forward them to ImGui.
		auto& App = FMonaApplication::Get();
		check(FMonaApplication::IsInitialized());
		App.SetMonaEventHandler(std::make_unique<FMonaBackendEventHandler>());
	}

	auto FMonaImGuiBackend::Shutdown() -> void
	{
		auto& App = FMonaApplication::Get();
		App.SetMonaEventHandler(nullptr);

		MonaImGuiBackend::ImGuiRHIImpl_Shutdown();
		MonaImGuiBackend::GMonaImGuiContext = nullptr;
		ImGui::DestroyContext();
	}

	auto FMonaImGuiBackend::NewFrame() -> void
	{
		MonaImGuiBackend::ImGuiRHIImpl_NewFrame();
	}

	auto FMonaImGuiBackend::Render() -> void
	{
		ImGui::Render();
	}

}