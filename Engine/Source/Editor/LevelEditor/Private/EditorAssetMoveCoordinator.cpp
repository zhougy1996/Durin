#include "EditorAssetMoveCoordinator.h"

#include "EditorSessionSettings.h"
#include "Engine/Level.h"
#include "LevelEditorContext.h"
#include "Misc/Project.h"
#include "Panels/SceneViewportPanel.h"

namespace Durin
{
	FEditorAssetMoveCoordinator::FEditorAssetMoveCoordinator(
		FLevelEditorContext& InContext,
		FEditorSessionSettings& InSessionSettings,
		FSceneViewportPanel& InSceneViewportPanel,
		std::string& InDefaultLevel,
		std::function<bool()> InSaveProjectSettings
	)
		: Context(InContext)
		, SessionSettings(InSessionSettings)
		, SceneViewportPanel(InSceneViewportPanel)
		, DefaultLevel(InDefaultLevel)
		, SaveProjectSettings(std::move(InSaveProjectSettings))
	{
	}

	auto FEditorAssetMoveCoordinator::MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> Asset::FAssetResult
	{
		const FEditorAssetMove Move{OldPath, NewPath};
		return MoveAssets(std::span{&Move, 1});
	}

	auto FEditorAssetMoveCoordinator::MoveAssets(std::span<const FEditorAssetMove> Moves) -> Asset::FAssetResult
	{
		if (Moves.empty()) return {};
		std::unordered_set<FAssetPath> Sources;
		std::unordered_set<FAssetPath> Destinations;
		for (const FEditorAssetMove& Move : Moves)
		{
			if (!Move.OldPath.IsValid() || !Move.NewPath.IsValid() || Move.OldPath == Move.NewPath)
				return {Asset::EAssetError::InvalidPath, "Asset move paths are invalid or identical."};
			if (!Sources.insert(Move.OldPath).second || !Destinations.insert(Move.NewPath).second)
				return {Asset::EAssetError::InvalidPath, "An asset move batch contains duplicate paths."};
		}

		if (Context.Level && Context.Level->GetPackage())
		{
			const std::string CurrentLevelPath = Context.Level->GetPackage()->GetPackagePath();
			if (std::ranges::any_of(Moves, [&CurrentLevelPath](const FEditorAssetMove& Move) { return Move.OldPath.ToString() == CurrentLevelPath; }))
				SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		}
		const FLevelViewportStateMap ViewportStatesBackup = SessionSettings.ViewportStates;
		const std::string DefaultLevelBackup = DefaultLevel;
		bool bViewportStateMoved = false;
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			const auto ProjectStates = ViewportStatesBackup.find(Project->ProjectFile);
			if (ProjectStates != ViewportStatesBackup.end())
				bViewportStateMoved = std::ranges::any_of(Moves, [&ProjectStates](const FEditorAssetMove& Move) { return ProjectStates->second.contains(Move.OldPath.ToString()); });
		}

		std::vector<FEditorAssetMove> CompletedMoves;
		CompletedMoves.reserve(Moves.size());
		for (const FEditorAssetMove& Move : Moves)
		{
			Asset::FAssetResult Result = Asset::MoveAsset(Move.OldPath, Move.NewPath);
			if (!Result)
			{
				const std::string RollbackError = RollbackAssets(CompletedMoves);
				if (!RollbackError.empty()) Result.Message += std::format(" Rollback also failed: {}", RollbackError);
				return Result;
			}
			CompletedMoves.push_back(Move);
		}

		bool bDefaultLevelMoved = false;
		for (const FEditorAssetMove& Move : Moves)
		{
			const std::string OldPath = Move.OldPath.ToString();
			const std::string NewPath = Move.NewPath.ToString();
			SessionSettings.MoveViewportState(OldPath, NewPath);
			if (DefaultLevel == OldPath)
			{
				DefaultLevel = NewPath;
				bDefaultLevelMoved = true;
			}
		}

		const bool bProjectSettingsSaved = !bDefaultLevelMoved || SaveProjectSettings();
		const bool bSessionSettingsSaved = bProjectSettingsSaved && (!bViewportStateMoved || SessionSettings.Save(&SceneViewportPanel));
		if (bProjectSettingsSaved && bSessionSettingsSaved) return {};

		// The editor metadata and asset paths form one logical operation. Restore both sides
		// before returning so callers cannot continue with a partially updated project.
		DefaultLevel = DefaultLevelBackup;
		SessionSettings.ViewportStates = ViewportStatesBackup;
		const std::string RollbackError = RollbackAssets(CompletedMoves);
		const bool bProjectSettingsRestored = !bDefaultLevelMoved || SaveProjectSettings();
		const bool bSessionSettingsRestored = !bViewportStateMoved || SessionSettings.Save(&SceneViewportPanel);

		std::string Message = bProjectSettingsSaved
			? "Could not save editor session settings after moving assets."
			: "Could not save project settings after moving the default level.";
		if (!RollbackError.empty()) Message += std::format(" Asset rollback also failed: {}", RollbackError);
		if (!bProjectSettingsRestored) Message += " The original project settings could not be restored.";
		if (!bSessionSettingsRestored) Message += " The original editor session settings could not be restored.";
		return {Asset::EAssetError::IoError, std::move(Message)};
	}

	auto FEditorAssetMoveCoordinator::RollbackAssets(std::span<const FEditorAssetMove> CompletedMoves) const -> std::string
	{
		std::string Errors;
		for (auto It = CompletedMoves.rbegin(); It != CompletedMoves.rend(); ++It)
		{
			const Asset::FAssetResult Result = Asset::MoveAsset(It->NewPath, It->OldPath);
			if (Result) continue;
			if (!Errors.empty()) Errors += ' ';
			Errors += Result.Message;
		}
		return Errors;
	}
} // namespace Durin
