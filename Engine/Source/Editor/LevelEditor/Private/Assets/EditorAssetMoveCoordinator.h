#pragma once

#include "Asset/MutationExtensions.h"

namespace Durin::Editor
{
	class FTransactionManager;
}

namespace Durin::Editor::Level
{
	class FLevelEditorSessionSettings;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	using FEditorAssetMove = Asset::FAssetRelocationMapping;

	// Extends an Engine Asset move with editor-owned state keyed by asset path.
	// Applies asset moves while keeping open documents and scene references coherent.
	class FEditorAssetMoveCoordinator final : public Asset::IAssetMoveObserver
	{
	public:
		FEditorAssetMoveCoordinator(
			FLevelEditorContext& InContext,
			FLevelEditorSessionSettings& InSessionSettings,
			FSceneViewportPanel& InSceneViewportPanel,
			::Durin::Editor::FTransactionManager& InTransactions,
			FModuleOwnedCallbackGate OwnerGate
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
