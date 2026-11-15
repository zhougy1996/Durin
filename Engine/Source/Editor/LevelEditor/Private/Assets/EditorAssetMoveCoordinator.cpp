#include "Assets/EditorAssetMoveCoordinator.h"

#include "DObject/Package.h"

#include "AssetTools/IAssetTools.h"
#include "Engine/Level.h"
#include "Settings/LevelEditorWorkspaceSettings.h"
#include "Workspace/LevelEditorContext.h"
#include "Panels/SceneViewportPanel.h"

namespace Durin::LevelEditor
{
	FEditorAssetMoveCoordinator::FEditorAssetMoveCoordinator(
		FLevelEditorContext& InContext,
		FLevelEditorWorkspaceSettings& InWorkspaceSettings,
		FSceneViewportPanel& InSceneViewportPanel
	)
		: Context(InContext)
		, WorkspaceSettings(InWorkspaceSettings)
		, SceneViewportPanel(InSceneViewportPanel)
	{
		ObserverHandle = RegisterAssetMoveObserver(this);
	}

	FEditorAssetMoveCoordinator::~FEditorAssetMoveCoordinator()
	{
		UnregisterAssetMoveObserver(ObserverHandle);
	}

	auto FEditorAssetMoveCoordinator::MoveAssets(std::span<const FEditorAssetMove> Moves, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (Moves.empty()) return true;
		if (Context.Level && Context.Level->GetPackage())
		{
			const std::string CurrentLevelPath = Context.Level->GetPackage()->GetPackagePath();
			if (std::ranges::any_of(Moves, [&CurrentLevelPath](const FEditorAssetMove& Move) { return Move.SourcePath.ToString() == CurrentLevelPath; }))
				WorkspaceSettings.CaptureViewportState(Context, SceneViewportPanel);
		}

		std::vector<FAssetRelocation> Mappings;
		Mappings.reserve(Moves.size());
		for (const FAssetRelocationMapping& Move : Moves)
			Mappings.push_back({Move.SourcePath, Move.DestinationPath});
		std::string ResultError;
		FFeedbackContext ResultFeedback({}, [&](ELogLevel Level, std::string_view Message) {
			if (Level >= ELogLevel::Error)
			{
				if (!ResultError.empty()) ResultError += "\n";
				ResultError += Message;
			}
		});
		const auto Result = IAssetTools::Get().RelocateAssets({
			.Mappings = std::move(Mappings)}, &ResultFeedback);
		if (!Result && OutError) *OutError = std::move(ResultError);
		return Result;
	}

	auto FEditorAssetMoveCoordinator::OnAssetsRelocated(
		std::span<const FAssetRelocationMapping> Mappings) -> void
	{
		for (const FAssetRelocationMapping& Mapping : Mappings)
			WorkspaceSettings.MoveViewportState(
				Mapping.SourcePath.ToString(), Mapping.DestinationPath.ToString());
		if (!Mappings.empty() && !WorkspaceSettings.Save(&SceneViewportPanel))
			DURIN_WARN_CATEGORY(
				"LevelEditor",
				"Could not persist transient viewport state after asset relocation.");
	}
} // namespace Durin::LevelEditor
