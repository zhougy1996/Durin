#include "Engine/AssetManagerTypes.h"
#include "Yaml/Yaml.h"
#include "Plugins/PluginResources.h"

namespace Durin
{
	namespace
	{
		auto ReadRules(const FYamlNodeView& Node, FPrimaryAssetRulesOverride& Rules) -> bool
		{
			if (!Node.IsValid()) return true;
			if (!Node.IsMap()) return false;
			for (const auto Key : {"Priority", "ChunkId"})
			{
				const auto Field = Node.GetView(Key);
				int64 Value = 0;
				if (!Field.IsValid()) continue;
				if (!Field.GetValue(Value) || Value < std::numeric_limits<int32>::min() || Value > std::numeric_limits<int32>::max()) return false;
				if (std::string_view(Key) == "Priority") Rules.Priority = static_cast<int32>(Value);
				else Rules.ChunkId = static_cast<int32>(Value);
			}
			if (const auto Field = Node.GetView("ApplyRecursively"); Field.IsValid())
			{
				bool Value = false;
				if (!Field.GetValue(Value)) return false;
				Rules.bApplyRecursively = Value;
			}
			if (const auto Field = Node.GetView("CookRule"); Field.IsValid())
			{
				const auto Value = Field.GetString();
				if (Value == "Unknown") Rules.CookRule = EPrimaryAssetCookRule::Unknown;
				else if (Value == "NeverCook") Rules.CookRule = EPrimaryAssetCookRule::NeverCook;
				else if (Value == "AlwaysCook") Rules.CookRule = EPrimaryAssetCookRule::AlwaysCook;
				else return false;
			}
			return true;
		}
		auto Strings(const FYamlNodeView& Node, std::vector<std::string>& Out) -> bool
		{
			if (!Node.IsValid()) return true;
			if (!Node.IsSequence()) return false;
			for (size_t Index = 0; Index < Node.Num(); ++Index)
			{
				std::string Value;
				if (!Node.GetView(Index).GetValue(Value) || Value.empty()) return false;
				Out.push_back(Value);
			}
			return true;
		}
	}
	auto FProjectAssetManagerSettingsStore::Load(FAssetManagerSettings& Out) const -> std::expected<void, std::string>
	{
		Out = {};
		FYamlDocument Document;
        std::string LoadError;
        if (!LoadProjectConfig(SettingsFile, Document, LoadError)) return std::unexpected(LoadError);
		auto Invalid = [&]() -> std::expected<void, std::string> {
			return std::unexpected("Invalid AssetManager settings in '" + SettingsFile.generic_string()
				+ "'; expected native types, valid IDs/paths, supported rules and redirect pairs.");
		};
		if (!Document.GetRootView().IsMap()) return Invalid();
		const auto Section = Document.GetRootView().GetView("AssetManager");
		if (!Section.IsValid()) return {};
		if (!Section.IsMap()) return Invalid();
		for (const auto Key : {"AcquireMissingChunksOnLoad", "bShouldAcquireMissingChunksOnLoad"})
			if (const auto Field = Section.GetView(Key); Field.IsValid())
			{
				bool bAcquire = false;
				if (!Field.GetValue(bAcquire) || bAcquire)
					return std::unexpected(std::string("AssetManager.") + Key + " requires an unsupported chunk/install backend.");
			}
		FAssetManagerSettings Settings;
		if (auto Class = Section.GetView("Class"); Class.IsValid() && !Class.GetValue(Settings.AssetManagerClass)) return Invalid();
		FPrimaryAssetRulesOverride DefaultRules;
		if (!ReadRules(Section.GetView("DefaultRules"), DefaultRules)) return Invalid();
		DefaultRules.Apply(Settings.DefaultRules);
		if (const auto Types = Section.GetView("Types"); Types.IsValid())
		{
			if (!Types.IsSequence()) return Invalid();
			std::unordered_set<FPrimaryAssetType> Seen;
			for (size_t Index = 0; Index < Types.Num(); ++Index)
			{
				const auto Node = Types.GetView(Index);
				if (!Node.IsMap()) return Invalid();
				FPrimaryAssetTypeInfo Type;
				Type.PrimaryAssetType = FPrimaryAssetType(FName(Node.GetView("Type").GetString()));
				if (!Type.PrimaryAssetType.IsValid() || !Seen.insert(Type.PrimaryAssetType).second) return Invalid();
				if (auto Field = Node.GetView("BaseClass"); Field.IsValid() && !Field.GetValue(Type.AssetBaseClass)) return Invalid();
				if (auto Field = Node.GetView("BlueprintClasses"); Field.IsValid() && (!Field.GetValue(Type.bHasBlueprintClasses) || Type.bHasBlueprintClasses)) return Invalid();
				if (auto Field = Node.GetView("EditorOnly"); Field.IsValid() && !Field.GetValue(Type.bIsEditorOnly)) return Invalid();
				std::vector<std::string> Assets;
				if (!Strings(Node.GetView("Directories"), Type.Directories) || !Strings(Node.GetView("Assets"), Assets)) return Invalid();
				for (auto& Directory : Type.Directories)
				{
					while (Directory.size() > 1 && Directory.back() == '/') Directory.pop_back();
					FPackagePath Validation;
					if (!FPackagePath::TryCreate(Directory + "/_Scan", Validation)) return Invalid();
				}
				for (const auto& Text : Assets)
				{
					FTopLevelAssetPath Path;
					if (!FTopLevelAssetPath::TryCreate(Text, Path)) return Invalid();
					Type.SpecificAssets.push_back(Path);
				}
				FPrimaryAssetRulesOverride Rules;
				if (!ReadRules(Node.GetView("Rules"), Rules)) return Invalid();
				Rules.Apply(Type.Rules);
				Settings.PrimaryAssetTypesToScan.push_back(std::move(Type));
			}
		}
		for (const auto Key : {"TypeRedirects", "IdRedirects", "AssetRules"})
		{
			const auto List = Section.GetView(Key);
			if (!List.IsValid()) continue;
			if (!List.IsSequence()) return Invalid();
			for (size_t Index = 0; Index < List.Num(); ++Index)
			{
				const auto Node = List.GetView(Index);
				if (!Node.IsMap()) return Invalid();
				const auto From = Node.GetView("From").GetString();
				const auto To = Node.GetView("To").GetString();
				if (std::string_view(Key) == "TypeRedirects")
				{
					if (From.empty() || To.empty() || From.find(':') != std::string::npos || To.find(':') != std::string::npos
						|| !Settings.PrimaryAssetTypeRedirects.emplace(FPrimaryAssetType(FName(From)), FPrimaryAssetType(FName(To))).second) return Invalid();
				}
				else
				{
					FPrimaryAssetId Id;
					if (std::string_view(Key) == "IdRedirects")
					{
						FPrimaryAssetId Target;
						if (!FPrimaryAssetId::TryParse(From, Id) || !FPrimaryAssetId::TryParse(To, Target)
							|| !Settings.PrimaryAssetIdRedirects.emplace(Id, Target).second) return Invalid();
					}
					else
					{
						FPrimaryAssetRulesOverride Rules;
						if (!FPrimaryAssetId::TryParse(Node.GetView("Id").GetString(), Id) || !ReadRules(Node.GetView("Rules"), Rules)
							|| !Settings.PrimaryAssetRules.emplace(Id, Rules).second) return Invalid();
					}
				}
			}
		}
		Out = std::move(Settings);
		return {};
	}
}
