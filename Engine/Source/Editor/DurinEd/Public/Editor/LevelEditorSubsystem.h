#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "DurinEdAPI.h"
#include "LevelEditorSubsystem.gen.h"

namespace Durin::LevelEditor { class FEditorModeTools; class FLevelEditorToolkit; }

namespace Durin
{
	struct FLevelEditorHostContext;
	// Per-host scripting entrypoints; the editor engine owns the actual PIE lifetime.
	DCLASS()
	class DLevelEditorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DLevelEditorSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto Initialize(FSubsystemCollection& Collection) -> void override;
		DURINED_API auto Deinitialize() noexcept -> void override;
		// Synchronously loads without a save prompt; true means the requested map is active.
		DFUNCTION(Category = "Level")
		DURINED_API auto LoadLevel(const std::string& AssetPath) -> bool;
		DFUNCTION(Category = "Play in Editor")
		DURINED_API auto IsInPlayInEditor() const -> bool;
		DFUNCTION(Category = "Play in Editor")
		DURINED_API auto EditorRequestBeginPlay() -> void;
		DFUNCTION(Category = "Play in Editor")
		DURINED_API auto EditorRequestEndPlay() -> void;
		DFUNCTION(Category = "Play in Editor")
		DURINED_API auto SetPlaySessionPaused(bool bPaused) -> bool;
		DFUNCTION(Category = "Play in Editor")
		DURINED_API auto IsPlaySessionPaused() const -> bool;
		auto SetHostContext(std::shared_ptr<FLevelEditorHostContext> Context) -> void { HostContext = std::move(Context); }
		auto GetHostContext() const -> std::shared_ptr<FLevelEditorHostContext> { return HostContext; }
		auto GetLevelEditorModeManager() const -> LevelEditor::FEditorModeTools* { return ModeTools; }
	private:
		friend class LevelEditor::FLevelEditorToolkit;
		LevelEditor::FEditorModeTools* ModeTools = nullptr;
		std::shared_ptr<FLevelEditorHostContext> HostContext;
		auto GetActiveEditor() const -> DEditorEngine*;
	};
}
