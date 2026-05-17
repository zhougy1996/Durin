#include "MonaImGuiBackend.h"

#include "ThirdParty/ImGui/imgui_threaded_rendering.h"

#include "RHI.h"
#include "RenderingThread.h"
#include "Rendering/MonaRHIRenderer.h"
#include "Application/MonaApplication.h"
#include "Widgets/MWindow.h"
#include "ImGuiRHIImpl.h"
#include "MonaImGuiEventHandler.h"

namespace Durin::Mona
{
	// Double buffer for draw data snapshots for the render thread.
	static std::array<ImDrawDataSnapshot, 2> GDrawDataSnapshots;

	auto FMonaImGuiBackend::Initialize() -> void
	{
		check(GDynamicRHI);
		MonaImGuiBackend::GMonaImGuiContext = ImGui::CreateContext();
		ImGui::SetCurrentContext(MonaImGuiBackend::GMonaImGuiContext);

		MonaImGuiBackend::ImGuiRHIImpl_Init();

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

		FlushRenderingCommands();
		for (ImDrawDataSnapshot& Snapshot : GDrawDataSnapshots)
		{
			Snapshot.Clear();
		}
		ImGui::DestroyContext();
	}

	auto FMonaImGuiBackend::NewFrame() -> void
	{
		auto& App = FMonaApplication::Get();
		const std::shared_ptr<MWindow> MainWindow = App.GetActiveTopLevelWindow();
		FVector2f ViewportSize = MainWindow->GetViewportSize();

		static double LastTime = FTime::Seconds();
		const double CurrentTime = FTime::Seconds();
		ImGui::GetIO().DeltaTime = static_cast<float>(CurrentTime - LastTime);
		ImGui::GetIO().DisplaySize = ImVec2(ViewportSize.x, ViewportSize.y);

		MonaImGuiBackend::ImGuiRHIImpl_NewFrame();
		ImGui::NewFrame();

		// Test content
		if (!MainWindow || MainWindow->IsMinimized())
		{
			return;
		}
		ImGui::Begin("Test Window");
		ImGui::Text("Hello from another window!");
		if (ImGui::Button("Close Me")) {}
		ImGui::End();
	}


	auto FMonaImGuiBackend::Render() -> void
	{
		ImGui::Render();

		//TODO: multiple windows
		ImDrawData* DrawData = ImGui::GetDrawData();

		auto& App = FMonaApplication::Get();
		const auto Renderer = dynamic_cast<FMonaRHIRenderer*>(App.GetRenderer());
		const std::shared_ptr<MWindow> MainWindow = App.GetActiveTopLevelWindow();
		if (!MainWindow || MainWindow->IsMinimized())
		{
			return;
		}

		FViewportRHIRef ViewportRHI = Renderer->GetRHIViewport(*MainWindow);

		ImDrawDataSnapshot& Snapshot = GDrawDataSnapshots[GFrameCounter % 2];
		Snapshot.SnapUsingSwap(DrawData, FTime::Seconds());

		MonaImGuiBackend::ImGuiRHIImpl_RenderDrawData(ViewportRHI, &Snapshot.DrawData);
	}

}