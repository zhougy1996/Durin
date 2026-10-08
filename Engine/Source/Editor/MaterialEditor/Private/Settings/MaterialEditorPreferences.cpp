#include "Widgets/MMaterialEditor.h"

#include "Settings/MaterialEditorSessionSettings.h"
#include "Widgets/MaterialEditingSession.h"
#include "Materials/Material.h"
#include "MonaImGui.h"

namespace Durin::Editor::Material
{
	namespace
	{
		// Snapshot only the exposed preferences, never the per-asset graph viewport map.
		struct FMaterialPreferences
		{
			bool bAutoCompile = true;
			bool bPreview = true;
			bool bDetails = true;
			bool bParameters = true;
			bool bDiagnostics = false;
		};
		auto Capture(const FMaterialEditorSessionSettings& Settings) -> FMaterialPreferences
		{
			return {Settings.bAutoCompile, Settings.bPreviewVisible, Settings.bDetailsVisible,
				Settings.bParametersVisible, Settings.bDiagnosticsVisible};
		}
		auto Apply(FMaterialEditorSessionSettings& Settings, const FMaterialPreferences& Values) -> void
		{
			Settings.bAutoCompile = Values.bAutoCompile;
			Settings.bPreviewVisible = Values.bPreview;
			Settings.bDetailsVisible = Values.bDetails;
			Settings.bParametersVisible = Values.bParameters;
			Settings.bDiagnosticsVisible = Values.bDiagnostics;
		}
	}

	auto MMaterialEditor::SetAutoCompile(bool bEnabled) -> void
	{
		SessionSettings->bAutoCompile = bEnabled;
		for (const auto& [Resource, Session] : EditingSessions)
			if (auto* Material = Session->GetWorkingMaterial())
				Material->SetEditCompileMode(bEnabled ? EMaterialEditCompileMode::Automatic : EMaterialEditCompileMode::Manual);
	}

	auto MMaterialEditor::DrawPreferences(std::string& OutError) -> void
	{
		const auto Previous = Capture(*SessionSettings);
		auto Next = Previous;
		ImGui::SeparatorText("Compilation");
		bool bChanged = ImGui::Checkbox("Auto compile after editing pauses", &Next.bAutoCompile);
		ImGui::SeparatorText("Panels");
		bChanged |= ImGui::Checkbox("Show preview", &Next.bPreview);
		bChanged |= ImGui::Checkbox("Show details", &Next.bDetails);
		bChanged |= ImGui::Checkbox("Show parameters", &Next.bParameters);
		bChanged |= ImGui::Checkbox("Show diagnostics", &Next.bDiagnostics);
		if (!bChanged) return;
		Apply(*SessionSettings, Next);
		if (!SessionSettings->Save())
		{
			Apply(*SessionSettings, Previous);
			OutError = "Could not save Material Editor preferences.";
			return;
		}
		SetAutoCompile(Next.bAutoCompile);
		OutError.clear();
	}

	auto MMaterialEditor::ResetPreferences() -> bool
	{
		const auto Previous = Capture(*SessionSettings);
		Apply(*SessionSettings, FMaterialPreferences{});
		if (!SessionSettings->Save()) { Apply(*SessionSettings, Previous); return false; }
		SetAutoCompile(true);
		return true;
	}
}
