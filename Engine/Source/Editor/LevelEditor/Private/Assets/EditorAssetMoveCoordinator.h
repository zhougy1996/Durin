#pragma once

#include "Asset/MutationExtensions.h"

namespace Durin::Editor::Level
{
	class FLevelEditorSessionSettings;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	using FEditorAssetMove = FAssetRelocationMapping;

	// Extends an Engine Asset move with editor-owned state keyed by asset path.
	// Applies asset moves while keeping open documents and scene references coherent.
	class FEditorAssetMoveCoordinator final : public IAssetMoveObserver
	{
	public:
		FEditorAssetMoveCoordinator(
			FLevelEditorContext& InContext,
			FLevelEditorSessionSettings& InSessionSettings,
			FSceneViewportPanel& InSceneViewportPanel
		);
		~FEditorAssetMoveCoordinator();

		auto MoveAssets(std::span<const FEditorAssetMove> Moves) -> FAssetResult;

	private:
		auto OnAssetsRelocated(
			std::span<const FAssetRelocationMapping> Mappings) -> void override;

		FLevelEditorContext& Context;
		FLevelEditorSessionSettings& SessionSettings;
		FSceneViewportPanel& SceneViewportPanel;
		FAssetMoveObserverHandle ObserverHandle = 0;
	};
} // namespace Durin::Editor::Level
