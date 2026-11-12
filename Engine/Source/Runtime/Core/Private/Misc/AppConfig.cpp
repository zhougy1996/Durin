#include "Misc/AppConfig.h"

#include "Yaml/Yaml.h"
#include "Json/Json.h"

namespace Durin
{
	namespace
	{
		auto GetAppConfigDocument() -> FYamlDocument&
		{
			static FYamlDocument AppConfigDocument;
			return AppConfigDocument;
		}

		FYamlNodeView GAppConfigRoot;
	}

    namespace
    {
        auto CopyConfigValue(FYamlNodeView Source, FJsonNodeRef Target, bool bMerge) -> void
        {
            if (Source.IsMap())
            {
                if (!bMerge || !Target.IsObject()) Target.EnsureObject();
                for (size_t I = 0; I < Source.Num(); ++I)
                {
                    const auto Child = Source.GetView(I);
                    const auto Key = Child.GetKey();
                    auto Destination = Target.GetRef(Key);
                    if (!Destination.IsValid()) Destination = Target.AddObject(Key);
                    CopyConfigValue(Child, Destination, bMerge);
                }
            }
            else if (Source.IsSequence())
            {
                Target.SetValue(""); // Sequences replace rather than append to lower layers.
                Target.EnsureArray();
                for (size_t I = 0; I < Source.Num(); ++I)
                    CopyConfigValue(Source.GetView(I), Target.AppendObject(), false);
            }
            else Target.SetValue(Source.GetString());
        }
    }
    auto LoadConfigLayers(std::span<const std::filesystem::path> Files,
        FYamlDocument& OutDocument, std::string& OutReason) -> bool
    {
        // Merge through owned JSON nodes; JSON is also valid YAML, and scalar
        // text retains the existing YAML typed-getter conversion semantics.
        FJsonDocument Merged;
        if (!Merged.Parse("{}")) { OutReason = "Cannot initialize configuration"; return false; }
        for (const auto& File : Files)
        {
            FYamlDocument Layer;
            const auto Loaded = Layer.LoadFromFile(File);
            if (!Loaded)
            {
                const auto* Error = std::get_if<FFileError>(&Loaded.error().Cause);
                if (Error && Error->NativeError == std::errc::no_such_file_or_directory) continue;
                OutReason = File.generic_string() + ": " + Loaded.error().ToString(); return false;
            }
            if (!Layer.GetRootView().IsMap()) { OutReason = File.generic_string() + ": configuration root must be a map"; return false; }
            try { CopyConfigValue(Layer.GetRootView(), Merged.GetMutableRoot(), true); }
            catch (const std::exception& Error) { OutReason = File.generic_string() + ": " + Error.what(); return false; }
        }
        FYamlDocument Candidate;
        if (const auto Parsed = Candidate.Parse(Merged.ToString()); !Parsed)
        { OutReason = Parsed.error().Message; return false; }
        OutDocument = std::move(Candidate); OutReason.clear(); return true;
    }
    auto LoadAppConfigLayers(std::span<const std::filesystem::path> Files, std::string& OutReason) -> bool
    {
        if (!LoadConfigLayers(Files, GetAppConfigDocument(), OutReason)) return false;
        GAppConfigRoot = GetAppConfigDocument().GetRootView();
        return true;
    }

	auto LoadAppConfig(std::string_view ConfigFile) -> bool
	{
		FYamlDocument& AppConfigDocument = GetAppConfigDocument();
		check(!AppConfigDocument.IsValid());

		if (const auto Loaded = AppConfigDocument.LoadFromFile(ConfigFile); !Loaded)
		{
			GAppConfigRoot = {};
			DURIN_ERROR("Failed to load application config file {}: {}", ConfigFile, Loaded.error().ToString());
			return false;
		}

		GAppConfigRoot = AppConfigDocument.GetRootView();
		return true;
	}

	auto GetModuleConfig(std::string_view ModuleName) -> FYamlNodeView
	{
		return GAppConfigRoot.GetView(ModuleName);
	}
}
