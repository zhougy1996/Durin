#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "LevelEditorAPI.h"
#include "LevelEditorSubsystem.gen.h"

namespace Durin
{
	struct FLevelEditorHostContext;
	DSTRUCT()
	struct FLevelEditorPlayResult
	{
		GENERATED_BODY()
		DPROPERTY()
		bool bSucceeded = false;
		DPROPERTY()
		std::string Message;
		DPROPERTY()
		std::string State;
	};

	// Per-host scripting entrypoints; the editor engine owns the actual PIE lifetime.
	DCLASS()
	class DLevelEditorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		LEVELEDITOR_API explicit DLevelEditorSubsystem(const FObjectInitializer& Initializer);
		auto Deinitialize() noexcept -> void override { HostContext.reset(); }
		DFUNCTION(Category = "Play in Editor")
		LEVELEDITOR_API auto GetPlayStatus() const -> FLevelEditorPlayResult;
		DFUNCTION(Category = "Play in Editor")
		LEVELEDITOR_API auto StartPlaySession() -> FLevelEditorPlayResult;
		DFUNCTION(Category = "Play in Editor")
		LEVELEDITOR_API auto StopPlaySession() -> FLevelEditorPlayResult;
		DFUNCTION(Category = "Play in Editor")
		LEVELEDITOR_API auto SetPlaySessionPaused(bool bPaused) -> FLevelEditorPlayResult;
		auto SetHostContext(std::shared_ptr<FLevelEditorHostContext> Context) -> void { HostContext = std::move(Context); }
		auto GetHostContext() const -> std::shared_ptr<FLevelEditorHostContext> { return HostContext; }
	private:
		std::shared_ptr<FLevelEditorHostContext> HostContext;
		auto Admit() const -> std::expected<DEditorEngine*, FLevelEditorPlayResult>;
	};
}
