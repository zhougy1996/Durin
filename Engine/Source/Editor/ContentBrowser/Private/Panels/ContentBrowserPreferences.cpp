#include "Panels/ContentBrowserPanel.h"

#include "MonaImGui.h"

namespace Durin::Editor::ContentBrowser::Private
{
	auto FContentBrowserPanel::ApplyPreferences(const FPresentationSettings& Next) -> bool
	{
		if (!SavePresentationSettings || !SavePresentationSettings(Next)) return false;
		ViewMode = static_cast<EContentBrowserViewMode>(Next.ViewMode);
		IconSize = Next.IconSize;
		bIconSizeLocked = Next.bIconSizeLocked;
		Model.SetShowHiddenFiles(Next.bShowHiddenFiles);
		PresentationSettings = Next;
		return true;
	}

	auto FContentBrowserPanel::DrawPreferences(std::string& OutError) -> void
	{
		FPresentationSettings Next = PresentationSettings;
		int Mode = static_cast<int>(ViewMode);
		Next.IconSize = IconSize;
		Next.bIconSizeLocked = bIconSizeLocked;
		Next.bShowHiddenFiles = Model.IsShowingHiddenFiles();
		Next.TreeWidth = DirectoryTreeWidth;
		Next.LastDirectory = Model.GetCurrentPhysicalPath();
		ImGui::SeparatorText("Presentation");
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(220.0f));
		bool bChanged = ImGui::Combo("View mode", &Mode, "Grid\0Details\0");
		Next.ViewMode = static_cast<uint8>(Mode);
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(220.0f));
		bChanged |= ImGui::SliderFloat("Thumbnail size", &Next.IconSize, FPresentationSettings::MinimumIconSize, FPresentationSettings::MaximumIconSize, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
		bChanged |= ImGui::Checkbox("Lock Ctrl + wheel resizing", &Next.bIconSizeLocked);
		bChanged |= ImGui::Checkbox("Show hidden files and folders", &Next.bShowHiddenFiles);
		if (bChanged) OutError = ApplyPreferences(Next) ? std::string{} : "Could not save Content Browser preferences.";
	}

	auto FContentBrowserPanel::ResetPreferences() -> bool
	{
		FPresentationSettings Next;
		Next.TreeWidth = DirectoryTreeWidth;
		Next.LastDirectory = Model.GetCurrentPhysicalPath();
		return ApplyPreferences(Next);
	}
}
