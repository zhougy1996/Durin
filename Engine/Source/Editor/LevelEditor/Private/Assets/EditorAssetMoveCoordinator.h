#pragma once

#include "CoreMinimal.h"

#include "AssetRenameData.h"
#include "AssetRegistry/Catalog.h"

namespace Durin::LevelEditor
{
	class FLevelEditorWorkspaceSettings;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	using FEditorAssetMove = FAssetRenameData;

	// Extends an Engine Asset move with editor-owned state keyed by asset path.
	// Applies asset moves while keeping open documents and scene references coherent.
	class FEditorAssetMoveCoordinator final
	{
	public:
		FEditorAssetMoveCoordinator(
			FLevelEditorContext& InContext,
			FLevelEditorWorkspaceSettings& InWorkspaceSettings,
			FSceneViewportPanel& InSceneViewportPanel
		);
		~FEditorAssetMoveCoordinator();

		auto MoveAssets(std::span<const FEditorAssetMove> Moves, std::string* OutError = nullptr) -> bool;

	private:
		auto OnAssetRenamed(const FAssetData& Asset, const FSoftObjectPath& Old) -> void;

		FLevelEditorContext& Context;
		FLevelEditorWorkspaceSettings& WorkspaceSettings;
		FSceneViewportPanel& SceneViewportPanel;
		FDelegateHandle RenameHandle;
	};
} // namespace Durin::LevelEditor
