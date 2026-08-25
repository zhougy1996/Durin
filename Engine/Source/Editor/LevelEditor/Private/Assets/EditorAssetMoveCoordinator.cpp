#include "Assets/EditorAssetMoveCoordinator.h"

#include "DObject/Package.h"

#include "Editor/AssetRelocation.h"
#include "Editor/Transaction.h"
#include "Engine/Level.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Workspace/LevelEditorContext.h"
#include "Panels/SceneViewportPanel.h"

namespace Durin::Editor::Level
{
	FEditorAssetMoveCoordinator::FEditorAssetMoveCoordinator(
		FLevelEditorContext& InContext,
		FLevelEditorSessionSettings& InSessionSettings,
		FSceneViewportPanel& InSceneViewportPanel,
		::Durin::Editor::FTransactionManager& InTransactions,
		FModuleOwnedCallbackGate OwnerGate
	)
		: Context(InContext)
		, SessionSettings(InSessionSettings)
		, SceneViewportPanel(InSceneViewportPanel)
		, Transactions(InTransactions)
	{
		ObserverHandle = Asset::RegisterAssetMoveObserver(this, std::move(OwnerGate));
	}

	FEditorAssetMoveCoordinator::~FEditorAssetMoveCoordinator()
	{
		Asset::UnregisterAssetMoveObserver(ObserverHandle);
	}

	auto FEditorAssetMoveCoordinator::MoveAssets(std::span<const FEditorAssetMove> Moves) -> Asset::FAssetResult
	{
		if (Moves.empty()) return {};
		if (Context.Level && Context.Level->GetPackage())
		{
			const std::string CurrentLevelPath = Context.Level->GetPackage()->GetPackagePath();
			if (std::ranges::any_of(Moves, [&CurrentLevelPath](const FEditorAssetMove& Move) { return Move.SourcePath.ToString() == CurrentLevelPath; }))
				SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		}

		return ::Durin::Editor::ExecuteAssetRelocations(Transactions, Moves);
	}

	auto FEditorAssetMoveCoordinator::OnAssetsRelocated(
		std::span<const Asset::FAssetRelocationMapping> Mappings) -> void
	{
		for (const Asset::FAssetRelocationMapping& Mapping : Mappings)
			SessionSettings.MoveViewportState(
				Mapping.SourcePath.ToString(), Mapping.DestinationPath.ToString());
		if (!Mappings.empty() && !SessionSettings.Save(&SceneViewportPanel))
			DURIN_WARN_CATEGORY(
				"LevelEditor",
				"Could not persist transient viewport state after asset relocation.");
	}
} // namespace Durin::Editor::Level
