#include "FMaterialEditor.h"
#include "MaterialEditorUI.h"

#include "Settings/MaterialEditorProfileSettings.h"
#include "Widgets/MaterialEditingSession.h"
#include "Materials/Material.h"
#include "MonaImGui.h"

namespace Durin::MaterialEditor
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
		auto Capture(const FMaterialEditorProfileSettings& Settings) -> FMaterialPreferences
		{
			return {Settings.bAutoCompile, Settings.bPreviewVisible, Settings.bDetailsVisible,
				Settings.bParametersVisible, Settings.bDiagnosticsVisible};
		}
		auto Apply(FMaterialEditorProfileSettings& Settings, const FMaterialPreferences& Values) -> void
		{
			Settings.bAutoCompile = Values.bAutoCompile;
			Settings.bPreviewVisible = Values.bPreview;
			Settings.bDetailsVisible = Values.bDetails;
			Settings.bParametersVisible = Values.bParameters;
			Settings.bDiagnosticsVisible = Values.bDiagnostics;
		}
	}

	auto FMaterialEditor::SetAutoCompile(bool bEnabled) -> void
	{
		ProfileSettings->bAutoCompile = bEnabled;
		if (const auto Session = EditingSession)
			if (auto* Material = Session->GetWorkingMaterial())
				Material->SetEditCompileMode(bEnabled ? EMaterialEditCompileMode::Automatic : EMaterialEditCompileMode::Manual);
	}

	auto DrawMaterialEditorPreferences(FMaterialEditorProfileSettings& Settings, std::string& OutError) -> bool
	{
		const auto Previous = Capture(Settings);
		auto Next = Previous;
		ImGui::SeparatorText("Compilation");
		bool bChanged = ImGui::Checkbox("Auto compile after editing pauses", &Next.bAutoCompile);
		ImGui::SeparatorText("Panels");
		bChanged |= ImGui::Checkbox("Show preview", &Next.bPreview);
		bChanged |= ImGui::Checkbox("Show details", &Next.bDetails);
		bChanged |= ImGui::Checkbox("Show parameters", &Next.bParameters);
		bChanged |= ImGui::Checkbox("Show diagnostics", &Next.bDiagnostics);
		if (!bChanged) return false;
		Apply(Settings, Next);
		if (!Settings.Save())
		{
			Apply(Settings, Previous);
			OutError = "Could not save Material Editor preferences.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto ResetMaterialEditorPreferences(FMaterialEditorProfileSettings& Settings) -> bool
	{
		const auto Previous = Capture(Settings);
		Apply(Settings, FMaterialPreferences{});
		if (!Settings.Save(true)) { Apply(Settings, Previous); return false; }
		return true;
	}

	auto FMaterialEditor::DrawPreferences(std::string& OutError) -> void
	{
		if (DrawMaterialEditorPreferences(*ProfileSettings, OutError)) SetAutoCompile(ProfileSettings->bAutoCompile);
	}

	auto FMaterialEditor::ResetPreferences() -> bool
	{
		if (!ResetMaterialEditorPreferences(*ProfileSettings)) return false;
		SetAutoCompile(true);
		return true;
	}
}
