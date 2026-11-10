#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "DObject/AssetBundleData.h"
#include "Asset/StreamableHandle.h"

namespace Durin
{
	enum class EPrimaryAssetCookRule : uint8 { Unknown, NeverCook, AlwaysCook };
	struct FPrimaryAssetRules
	{
		int32 Priority = 0;
		int32 ChunkId = -1;
		bool bApplyRecursively = true;
		EPrimaryAssetCookRule CookRule = EPrimaryAssetCookRule::Unknown;
		auto operator==(const FPrimaryAssetRules&) const -> bool = default;
	};
	struct FPrimaryAssetRulesOverride
	{
		std::optional<int32> Priority, ChunkId;
		std::optional<bool> bApplyRecursively;
		std::optional<EPrimaryAssetCookRule> CookRule;
		auto Apply(FPrimaryAssetRules& Rules) const -> void
		{
			if (Priority) Rules.Priority = *Priority;
			if (ChunkId) Rules.ChunkId = *ChunkId;
			if (bApplyRecursively) Rules.bApplyRecursively = *bApplyRecursively;
			if (CookRule) Rules.CookRule = *CookRule;
		}
	};
	struct FPrimaryAssetTypeInfo
	{
		FPrimaryAssetType PrimaryAssetType;
		std::string AssetBaseClass;
		std::vector<std::string> Directories;
		std::vector<FTopLevelAssetPath> SpecificAssets;
		bool bIsEditorOnly = false;
		bool bHasBlueprintClasses = false;
		FPrimaryAssetRules Rules;
	};
	struct FAssetManagerSettings
	{
		std::string AssetManagerClass;
		FPrimaryAssetRules DefaultRules;
		std::vector<FPrimaryAssetTypeInfo> PrimaryAssetTypesToScan;
		std::unordered_map<FPrimaryAssetType, FPrimaryAssetType> PrimaryAssetTypeRedirects;
		std::unordered_map<FPrimaryAssetId, FPrimaryAssetId> PrimaryAssetIdRedirects;
		std::unordered_map<FPrimaryAssetId, FPrimaryAssetRulesOverride> PrimaryAssetRules;
	};
	struct FAssetManagerLoadParams
	{
		FStreamableDelegate OnComplete;
		FStreamableDelegate OnCancel;
		int32 Priority = 0;
		std::string DebugName;
	};
	// Reads only AssetManager; Game settings edits preserve this section.
	class FProjectAssetManagerSettingsStore
	{
	public:
		explicit FProjectAssetManagerSettingsStore(std::filesystem::path File) : SettingsFile(std::move(File)) {}
		ENGINE_API auto Load(FAssetManagerSettings& Out) const -> std::expected<void, std::string>;
	private:
		std::filesystem::path SettingsFile;
	};
}
