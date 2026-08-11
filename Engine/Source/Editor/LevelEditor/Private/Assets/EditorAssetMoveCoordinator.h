#pragma once

#include "AssetSystem.h"

namespace Durin::Editor
{
	class FTransactionManager;
}

namespace Durin::Editor::Level
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
	class FEditorAssetMoveCoordinator final : public Asset::IAssetMoveObserver
	{
	public:
		FEditorAssetMoveCoordinator(
			FLevelEditorContext& InContext,
			FLevelEditorSessionSettings& InSessionSettings,
			FSceneViewportPanel& InSceneViewportPanel,
			::Durin::Editor::FTransactionManager& InTransactions
		);
		~FEditorAssetMoveCoordinator();

		auto MoveAssets(std::span<const FEditorAssetMove> Moves) -> Asset::FAssetResult;

	private:
		auto OnAssetsRelocated(
			std::span<const Asset::FAssetRelocationMapping> Mappings) -> void override;

		FLevelEditorContext& Context;
		FLevelEditorSessionSettings& SessionSettings;
		FSceneViewportPanel& SceneViewportPanel;
		::Durin::Editor::FTransactionManager& Transactions;
		Asset::FAssetMoveObserverHandle ObserverHandle = 0;
	};
} // namespace Durin::Editor::Level
