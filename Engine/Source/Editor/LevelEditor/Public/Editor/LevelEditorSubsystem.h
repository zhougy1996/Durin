#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "LevelEditorAPI.h"
#include "LevelEditorSubsystem.gen.h"

namespace Durin
{
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
	DCLASS(NoClassDefaultObject)
	class DLevelEditorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		LEVELEDITOR_API explicit DLevelEditorSubsystem(const FObjectInitializer& Initializer);
		DFUNCTION(Category = "Play in Editor")
		LEVELEDITOR_API auto GetPlayStatus() const -> FLevelEditorPlayResult;
		DFUNCTION(Category = "Play in Editor")
		LEVELEDITOR_API auto StartPlaySession() -> FLevelEditorPlayResult;
		DFUNCTION(Category = "Play in Editor")
		LEVELEDITOR_API auto StopPlaySession() -> FLevelEditorPlayResult;
		DFUNCTION(Category = "Play in Editor")
		LEVELEDITOR_API auto SetPlaySessionPaused(bool bPaused) -> FLevelEditorPlayResult;
	private:
		auto Admit() const -> std::expected<DEditorEngine*, FLevelEditorPlayResult>;
	};
}
