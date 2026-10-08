#include "Widgets/MLevelEditor.h"

#include "Panels/SceneViewportPanel.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "MonaImGui.h"

namespace Durin::Editor::Level
{
	namespace
	{
		struct FViewportPreferences
		{
			float CameraSpeed = 5.0f;
			bool bGrid = true;
			bool bStatistics = false;
			FTransformGizmoSnapSettings Snap;
		};

		auto Capture(FSceneViewportPanel& Panel) -> FViewportPreferences
		{
			FViewportPreferences Values;
			Values.CameraSpeed = Panel.GetCameraMovementSpeed();
			Values.bGrid = Panel.IsGridVisible();
			Values.bStatistics = Panel.IsStatisticsVisible();
			if (const auto* Gizmo = Panel.GetTransformGizmo()) Values.Snap = Gizmo->GetSnapSettings();
			return Values;
		}

		auto Apply(FSceneViewportPanel& Panel, const FViewportPreferences& Values) -> void
		{
			Panel.SetCameraMovementSpeed(Values.CameraSpeed);
			Panel.SetGridVisible(Values.bGrid);
			Panel.SetStatisticsVisible(Values.bStatistics);
			if (auto* Gizmo = Panel.GetTransformGizmo()) Gizmo->GetSnapSettings() = Values.Snap;
		}
	}

	auto MLevelEditor::DrawViewportPreferences(std::string& OutError) -> void
	{
		if (!SceneViewportPanel) { ImGui::TextDisabled("The level viewport is unavailable."); return; }
		const auto Previous = Capture(*SceneViewportPanel);
		auto Next = Previous;
		ImGui::SeparatorText("Navigation");
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(220.0f));
		bool bChanged = ImGui::DragFloat("Camera movement speed", &Next.CameraSpeed, 0.05f, 0.05f, 10000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SeparatorText("Overlays");
		bChanged |= ImGui::Checkbox("Show world grid", &Next.bGrid);
		bChanged |= ImGui::Checkbox("Show viewport statistics", &Next.bStatistics);
		ImGui::SeparatorText("Transform snapping");
		bChanged |= ImGui::Checkbox("Enable snapping", &Next.Snap.bEnabled);
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(220.0f));
		bChanged |= ImGui::DragFloat("Translation step", &Next.Snap.Translation, 0.05f, 0.001f, 10000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(220.0f));
		bChanged |= ImGui::DragFloat("Rotation step (degrees)", &Next.Snap.RotationDegrees, 0.5f, 0.01f, 360.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(220.0f));
		bChanged |= ImGui::DragFloat("Scale step", &Next.Snap.Scale, 0.01f, 0.001f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (!bChanged) return;
		Apply(*SceneViewportPanel, Next);
		if (!SessionSettings.Save(SceneViewportPanel))
		{
			Apply(*SceneViewportPanel, Previous);
			OutError = "Could not save viewport preferences.";
		}
		else OutError.clear();
	}

	auto MLevelEditor::ResetViewportPreferences() -> bool
	{
		if (!SceneViewportPanel) return false;
		const auto Previous = Capture(*SceneViewportPanel);
		Apply(*SceneViewportPanel, FViewportPreferences{});
		if (SessionSettings.Save(SceneViewportPanel)) return true;
		Apply(*SceneViewportPanel, Previous);
		return false;
	}
}
