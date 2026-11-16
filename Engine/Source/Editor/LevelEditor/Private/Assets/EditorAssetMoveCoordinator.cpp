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
		RenameHandle = Durin::OnAssetRenamed().AddLambda([this](const FAssetData& Asset, const FSoftObjectPath& Old) { OnAssetRenamed(Asset, Old); });
	}

	FEditorAssetMoveCoordinator::~FEditorAssetMoveCoordinator()
	{
		Durin::OnAssetRenamed().Remove(RenameHandle);
	}

	auto FEditorAssetMoveCoordinator::MoveAssets(std::span<const FEditorAssetMove> Moves, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (Moves.empty()) return true;
		if (Context.Level && Context.Level->GetPackage())
		{
			if (std::ranges::any_of(Moves, [this](const FEditorAssetMove& Move) { return Move.Asset.Get() == Context.Level; }))
				WorkspaceSettings.CaptureViewportState(Context, SceneViewportPanel);
		}

		std::string ResultError;
		auto& FallbackFeedback = FFeedbackContext::Get();
		FFeedbackContext ResultFeedback({}, [&](ELogLevel Level, std::string_view Message) {
			if (Level >= ELogLevel::Error)
			{
				if (!ResultError.empty()) ResultError += "\n";
				ResultError += Message;
			}
			FallbackFeedback.Serialize(Level, Message);
		});
		FScopedFeedbackContext FeedbackScope(ResultFeedback);
		const auto Result = IAssetTools::Get().RenameAssets(Moves);
		if (OutError) *OutError = std::move(ResultError);
		return Result;
	}

	auto FEditorAssetMoveCoordinator::OnAssetRenamed(const FAssetData& Asset, const FSoftObjectPath& Old) -> void
	{
		if (!Context.Level || Asset.AssetPath.ToString() != Context.Level->GetObjectPath()) return;
		WorkspaceSettings.MoveViewportState(Old.GetPackagePath().ToString(), Asset.AssetPath.GetPackagePath().ToString());
		if (!WorkspaceSettings.Save(&SceneViewportPanel)) DURIN_WARN_CATEGORY("LevelEditor", "Could not persist viewport state after asset rename.");
	}
}
