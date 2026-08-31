#include "Settings/HostSettings.h"

#include "Application/GenericApplication.h"
#include "Misc/Paths.h"
#include "Yaml/Yaml.h"

namespace Durin::Editor::MainFrame
{
	namespace
	{
		constexpr const char* HostSettingsFileName = "EditorHostSettings.yaml";

		auto LoadDisplaySettings(const FYamlNodeView& Display, FHostSettings& Settings) -> void
		{
			Settings.SetDisplaySettings(
				static_cast<int32>(Display.GetView("WindowWidth").GetInt(Settings.GetWindowWidth())),
				static_cast<int32>(Display.GetView("WindowHeight").GetInt(Settings.GetWindowHeight())),
				static_cast<float>(Display.GetView("UIScale").GetDouble(Settings.GetUIScale()))
			);
			Settings.SetColorTheme(Display.GetView("ColorTheme").GetString("Dark") == "Light"
				? MonaImGui::EColorTheme::Light : MonaImGui::EColorTheme::Dark);
			Settings.SetWindowMaximized(Display.GetView("WindowMaximized").GetBool(true));
		}
	}

	auto FHostSettings::Load() -> bool
	{
		const std::vector<FMonitorInfo> Monitors = EnumerateMonitors();
		if (!Monitors.empty())
		{
			WindowWidth = std::min(1600, static_cast<int32>(Monitors.front().WorkSize.x * 0.9f));
			WindowHeight = std::min(1000, static_cast<int32>(Monitors.front().WorkSize.y * 0.9f));
			UIScale = Monitors.front().WorkSize.y >= 1800 ? 1.5f : Monitors.front().WorkSize.y >= 1300 ? 1.25f : 1.0f;
		}

		const std::string HostPath = FPaths::LaunchConfigsDir() + HostSettingsFileName;
		if (!std::filesystem::exists(HostPath)) return true;

		FYamlDocument Document;
		FYamlParseError Error;
		if (!Document.LoadFromFile(HostPath, &Error))
		{
			DURIN_WARN("Failed to load editor host settings: {}", Error.Message);
			return false;
		}
		LoadDisplaySettings(Document.GetRootView().GetView("Display"), *this);
		return true;
	}

	auto FHostSettings::Save() const -> bool
	{
		FYamlDocument Document;
		FYamlNodeRef Display = Document.GetMutableRoot().AddMap("Display");
		Display.SetChildValue("WindowWidth", WindowWidth);
		Display.SetChildValue("WindowHeight", WindowHeight);
		Display.SetChildValue("UIScale", static_cast<double>(UIScale));
		Display.SetChildValue("ColorTheme", ColorTheme == MonaImGui::EColorTheme::Light ? "Light" : "Dark");
		Display.SetChildValue("WindowMaximized", bWindowMaximized);
		if (!Document.SaveToFile(FPaths::LaunchConfigsDir() + HostSettingsFileName))
		{
			DURIN_WARN("Failed to save editor host settings.");
			return false;
		}
		return true;
	}

	auto FHostSettings::SetDisplaySettings(int32 Width, int32 Height, float Scale) -> void
	{
		WindowWidth = Width;
		WindowHeight = Height;
		UIScale = std::clamp(Scale, 0.75f, 2.0f);
	}
}
