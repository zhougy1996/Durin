#include "ContentBrowser/ContentBrowserTool.h"

#include "Misc/Paths.h"
#include "Yaml/Yaml.h"

namespace Durin::Editor::ContentBrowser
{
	namespace
	{
		constexpr const char* SettingsFileName = "ContentBrowserSettings.yaml";

		auto LoadValues(const FYamlNodeView& Node) -> FPresentationSettings
		{
			FPresentationSettings Settings;
			Settings.ViewMode = static_cast<uint8>(std::clamp<int64>(
				Node.GetView("ViewMode").GetInt(0), 0, 1));
			const double IconSize = Node.GetView("IconSize").GetDouble(
				FPresentationSettings::DefaultIconSize);
			Settings.IconSize = std::isfinite(IconSize)
				? static_cast<float>(std::clamp(IconSize,
					static_cast<double>(FPresentationSettings::MinimumIconSize),
					static_cast<double>(FPresentationSettings::MaximumIconSize)))
				: FPresentationSettings::DefaultIconSize;
			Settings.bIconSizeLocked =
				Node.GetView("IconSizeLocked").GetBool(false);
			const double TreeWidth = Node.GetView("TreeWidth").GetDouble(
				FPresentationSettings::DefaultTreeRatio);
			Settings.TreeWidth = std::isfinite(TreeWidth)
				? static_cast<float>(std::clamp(TreeWidth,
					static_cast<double>(FPresentationSettings::MinimumTreeRatio),
					static_cast<double>(FPresentationSettings::MaximumTreeRatio)))
				: FPresentationSettings::DefaultTreeRatio;
			Settings.bShowHiddenFiles =
				Node.GetView("ShowHiddenFiles").GetBool(false);
			Settings.LastDirectory =
				Node.GetView("LastDirectory").GetString();
			return Settings;
		}
	}

	auto SavePresentationSettings(const FPresentationSettings& Settings) -> bool
	{
		FYamlDocument Document;
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		Root.SetChildValue("ViewMode", static_cast<int64>(Settings.ViewMode));
		Root.SetChildValue("IconSize", static_cast<double>(Settings.IconSize));
		Root.SetChildValue("IconSizeLocked", Settings.bIconSizeLocked);
		Root.SetChildValue("TreeWidth", static_cast<double>(Settings.TreeWidth));
		Root.SetChildValue("ShowHiddenFiles", Settings.bShowHiddenFiles);
		Root.SetChildValue("LastDirectory", Settings.LastDirectory);
		return Document.SaveToFile(
			FPaths::LaunchConfigsDir() + SettingsFileName);
	}

	auto LoadPresentationSettings(
		FPresentationSettings& Settings, std::string* OutWarning) -> bool
	{
		Settings = {};
		const std::string SettingsPath =
			FPaths::LaunchConfigsDir() + SettingsFileName;
		if (std::filesystem::exists(SettingsPath))
		{
			FYamlDocument Document;
			FYamlParseError Error;
			if (!Document.LoadFromFile(SettingsPath, &Error))
			{
				if (OutWarning) *OutWarning = Error.Message;
				return false;
			}
			Settings = LoadValues(Document.GetRootView());
			return true;
		}

		if (SavePresentationSettings(Settings)) return true;
		if (OutWarning && OutWarning->empty())
			*OutWarning = "Could not save Content Browser settings.";
		return false;
	}
} // namespace Durin::Editor::ContentBrowser
