#include "Settings/EditorSettingsWindow.h"

#include "Settings/EditorSettings.h"
#include "Settings/HostSettings.h"
#include "Mona.h"
#include "Rendering/MonaRenderer.h"
#include "Widgets/MWindow.h"

namespace Durin::Editor::MainFrame
{
	namespace
	{
		auto ApplyAppearance(const FHostSettings& Settings, MWindow& Window) -> void
		{
			MonaImGui::SetColorTheme(Settings.GetColorTheme());
			MonaImGui::SetGlobalUIScale(Settings.GetUIScale());
			if (!Window.IsMaximized()) Window.ResizeWindow({
				static_cast<float>(Settings.GetWindowWidth()), static_cast<float>(Settings.GetWindowHeight())});
		}

		auto DrawAppearance(FHostSettings& Settings, MWindow& Window, std::string& Error) -> void
		{
			FHostSettings Next = Settings;
			int Theme = Settings.GetColorTheme() == MonaImGui::EColorTheme::Light ? 1 : 0;
			ImGui::SeparatorText("Theme and scale");
			ImGui::SetNextItemWidth(MonaImGui::ScaleUI(200.0f));
			bool bChanged = ImGui::Combo("Color theme", &Theme, "Dark\0Light\0");
			Next.SetColorTheme(Theme == 1 ? MonaImGui::EColorTheme::Light : MonaImGui::EColorTheme::Dark);
			const float Scales[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
			const std::string ScaleLabel = std::format("{}%", static_cast<int32>(Settings.GetUIScale() * 100.0f));
			ImGui::SetNextItemWidth(MonaImGui::ScaleUI(200.0f));
			if (ImGui::BeginCombo("UI scale", ScaleLabel.c_str()))
			{
				for (const float Scale : Scales)
				{
					const std::string Label = std::format("{}%", static_cast<int32>(Scale * 100.0f));
					if (ImGui::Selectable(Label.c_str(), std::abs(Settings.GetUIScale() - Scale) < 0.01f))
					{
						Next.SetDisplaySettings(Settings.GetWindowWidth(), Settings.GetWindowHeight(), Scale);
						bChanged = true;
					}
				}
				ImGui::EndCombo();
			}
			if (!bChanged) return;
			if (!Next.Save()) { Error = "Could not save editor appearance settings."; return; }
			Settings = Next;
			ApplyAppearance(Settings, Window);
			Error.clear();
		}
	}

	auto FEditorSettingsWindow::Draw(FHostSettings& Settings, MWindow& RootWindow) -> void
	{
		auto Pages = FEditorSettingsRegistry::Get().GetPages();
		Pages.insert(Pages.begin(), {
			{"editor.appearance", "Appearance", "theme dark light scale font UI", "Theme and interface scale. Changes take effect immediately.", false,
				[&](std::string& OutError) { DrawAppearance(Settings, RootWindow, OutError); },
				[&] {
					FHostSettings Next = Settings;
					Next.SetColorTheme(MonaImGui::EColorTheme::Dark);
					Next.SetDisplaySettings(Settings.GetWindowWidth(), Settings.GetWindowHeight(), 1.0f);
					if (!Next.Save()) return false;
					Settings = Next;
					ApplyAppearance(Settings, RootWindow);
					return true;
				}, {}},
			{"editor.display", "Display", "performance vsync vertical synchronization", "Presentation settings for editor windows. Changes take effect immediately.", false,
				[&](std::string& OutError) {
					bool bVSync = Settings.IsVSyncEnabled();
					ImGui::SeparatorText("Presentation");
					if (!ImGui::Checkbox("Vertical synchronization (VSync)", &bVSync)) return;
					FHostSettings Next = Settings;
					Next.SetVSyncEnabled(bVSync);
					if (!Next.Save()) { OutError = "Could not save display settings."; return; }
					Settings = Next;
					if (auto* Renderer = Mona::FMonaApplication::Get().GetRenderer())
						Renderer->SetPresentationPolicyOverride(bVSync ? EViewportPresentationPolicy::FramePaced : EViewportPresentationPolicy::Unsynchronized);
					OutError.clear();
				},
				[&] {
					FHostSettings Next = Settings;
					Next.SetVSyncEnabled(true);
					if (!Next.Save()) return false;
					Settings = Next;
					if (auto* Renderer = Mona::FMonaApplication::Get().GetRenderer())
						Renderer->SetPresentationPolicyOverride(EViewportPresentationPolicy::FramePaced);
					return true;
				}, {}}
		});
		if (const auto Request = FEditorSettingsRegistry::Get().TakeOpenRequest())
		{
			bOpen = true;
			Search.Clear();
			if (!Request->empty()) SelectedPage = *Request;
			Error.clear();
			for (auto& Page : Pages) if (Page.Id == SelectedPage && Page.OnOpen) Page.OnOpen();
		}
		if (!bOpen) return;
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(900.0f), MonaImGui::ScaleUI(600.0f)), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(MonaImGui::ScaleUI(680.0f), MonaImGui::ScaleUI(420.0f)), ImVec2(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()));
		if (ImGui::Begin("Settings###Durin.EditorHost.Settings", &bOpen, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse))
		{
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputTextWithHint("##SettingsSearch", "Search settings...", Search.InputBuf, std::size(Search.InputBuf))) Search.Build();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search categories and setting names. Separate terms with commas; prefix exclusions with '-'.");
			ImGui::Separator();
			// ImGui returns on the first matching alternative; exclusions must take precedence.
			std::stable_partition(Search.Filters.begin(), Search.Filters.end(), [](const auto& Term) {
				return Term.b != Term.e && *Term.b == '-';
			});
			std::vector<const FEditorSettingsPage*> Visible;
			for (const auto& Page : Pages)
			{
				const std::string SearchText = Page.Label + " " + Page.Keywords + " " + Page.Description + (Page.bProject ? " project" : " editor");
				if (Search.PassFilter(SearchText.c_str())) Visible.push_back(&Page);
			}
			if (!Visible.empty() && std::ranges::none_of(Visible, [&](const auto* Page) { return Page->Id == SelectedPage; }))
			{
				SelectedPage = Visible.front()->Id;
				Error.clear();
				if (Visible.front()->OnOpen) Visible.front()->OnOpen();
			}
			const float FooterHeight = ImGui::GetFrameHeightWithSpacing() + MonaImGui::ScaleUI(10.0f);
			if (ImGui::BeginChild("Categories", ImVec2(MonaImGui::ScaleUI(205.0f), -FooterHeight), ImGuiChildFlags_Borders))
			{
				for (const bool bProject : {false, true})
				{
					ImGui::SeparatorText(bProject ? "Project" : "Editor");
					for (const auto* Page : Visible)
					{
						if (Page->bProject != bProject) continue;
						ImGui::PushID(Page->Id.c_str());
						if (ImGui::Selectable(Page->Label.c_str(), SelectedPage == Page->Id) && SelectedPage != Page->Id)
						{
							SelectedPage = Page->Id;
							Error.clear();
							if (Page->OnOpen) Page->OnOpen();
						}
						ImGui::PopID();
					}
				}
			}
			ImGui::EndChild();
			ImGui::SameLine();
			const std::string ContentId = "SettingsContent###Durin.Settings." + SelectedPage;
			if (ImGui::BeginChild(ContentId.c_str(), ImVec2(0.0f, -FooterHeight)))
			{
				if (Visible.empty()) ImGui::TextDisabled("No matching settings. Try another search.");
				for (const auto* Page : Visible)
				{
					if (Page->Id != SelectedPage) continue;
					ImGui::PushID(Page->Id.c_str());
					ImGui::SeparatorText(Page->Label.c_str());
					ImGui::TextDisabled("%s", Page->bProject ? "Scope: current project" : "Scope: local editor preferences");
					ImGui::TextWrapped("%s", Page->Description.c_str());
					ImGui::Spacing();
					Page->Draw(Error);
					ImGui::Spacing();
					if (Page->Reset && ImGui::Button("Restore page defaults"))
						Error = Page->Reset() ? std::string{} : "Could not save the default settings. Please check the configuration directory.";
					ImGui::PopID();
				}
				if (!Error.empty())
				{
					ImGui::PushStyleColor(ImGuiCol_Text, MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error));
					ImGui::TextWrapped("%s", Error.c_str());
					ImGui::PopStyleColor();
				}
			}
			ImGui::EndChild();
			ImGui::Separator();
			ImGui::TextDisabled("Preferences save automatically. Project changes use Apply.");
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - MonaImGui::ScaleUI(62.0f));
			if (ImGui::Button("Close")) bOpen = false;
		}
		ImGui::End();
	}
}
