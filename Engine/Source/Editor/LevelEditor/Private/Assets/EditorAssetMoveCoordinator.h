#pragma once

#include "AssetSystem.h"

namespace Durin
{
	class FLevelEditorSessionSettings;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	// Describes one asset relocation from its current resource path.
	struct FEditorAssetMove
	{
		FAssetPath OldPath;
		FAssetPath NewPath;
	};

	// Extends an AssetCore move with the editor-owned state keyed by asset path.
	// Applies asset moves while keeping open documents and scene references coherent.
	class FEditorAssetMoveCoordinator
	{
	public:
		FEditorAssetMoveCoordinator(
			FLevelEditorContext& InContext,
			FLevelEditorSessionSettings& InSessionSettings,
			FSceneViewportPanel& InSceneViewportPanel,
			std::string& InDefaultLevel,
			std::function<bool()> InSaveProjectSettings
		);

		auto MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> Asset::FAssetResult;
		auto MoveAssets(std::span<const FEditorAssetMove> Moves) -> Asset::FAssetResult;

	private:
		auto RollbackAssets(std::span<const FEditorAssetMove> CompletedMoves) const -> std::string;

		FLevelEditorContext& Context;
		FLevelEditorSessionSettings& SessionSettings;
		FSceneViewportPanel& SceneViewportPanel;
		std::string& DefaultLevel;
		std::function<bool()> SaveProjectSettings;
	};
} // namespace Durin
