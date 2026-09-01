#include "Assets/EditorAssetMoveCoordinator.h"

#include "DObject/Package.h"

#include "AssetTools/IAssetTools.h"
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
		FModuleOwnedCallbackGate OwnerGate
	)
		: Context(InContext)
		, SessionSettings(InSessionSettings)
		, SceneViewportPanel(InSceneViewportPanel)
	{
		ObserverHandle = RegisterAssetMoveObserver(this, std::move(OwnerGate));
	}

	FEditorAssetMoveCoordinator::~FEditorAssetMoveCoordinator()
	{
		UnregisterAssetMoveObserver(ObserverHandle);
	}

	auto FEditorAssetMoveCoordinator::MoveAssets(std::span<const FEditorAssetMove> Moves) -> FAssetResult
	{
		if (Moves.empty()) return {};
		if (Context.Level && Context.Level->GetPackage())
		{
			const std::string CurrentLevelPath = Context.Level->GetPackage()->GetPackagePath();
			if (std::ranges::any_of(Moves, [&CurrentLevelPath](const FEditorAssetMove& Move) { return Move.SourcePath.ToString() == CurrentLevelPath; }))
				SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		}

		std::vector<FAssetRelocation> Mappings;
		Mappings.reserve(Moves.size());
		for (const FAssetRelocationMapping& Move : Moves)
			Mappings.push_back({Move.SourcePath, Move.DestinationPath});
		const FAssetOperationResult Result = IAssetTools::Get().RelocateAssets({
			.Mappings = std::move(Mappings)});
		return Result ? FAssetResult{}
			: FAssetResult{EAssetError::IoError, Result.Message};
	}

	auto FEditorAssetMoveCoordinator::OnAssetsRelocated(
		std::span<const FAssetRelocationMapping> Mappings) -> void
	{
		for (const FAssetRelocationMapping& Mapping : Mappings)
			SessionSettings.MoveViewportState(
				Mapping.SourcePath.ToString(), Mapping.DestinationPath.ToString());
		if (!Mappings.empty() && !SessionSettings.Save(&SceneViewportPanel))
			DURIN_WARN_CATEGORY(
				"LevelEditor",
				"Could not persist transient viewport state after asset relocation.");
	}
} // namespace Durin::Editor::Level
