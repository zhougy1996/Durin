#pragma once

#include "DObject/ObjectPtr.h"

namespace Durin
{
	class AActor;
	class DWorld;
	class DLevel;

	struct FLevelEditorContext
	{
		DWorld* World = nullptr;
		DLevel* Level = nullptr;
		TObjectPtr<AActor> SelectedActor;
		std::function<void(std::string)> ReportError;

		auto Synchronize(DWorld* CurrentWorld) -> void;
		auto SelectActor(AActor* Actor) -> void;
		auto ClearSelection() -> void { SelectedActor = nullptr; }
		auto SetError(std::string Message) const -> void { if (ReportError) ReportError(std::move(Message)); }
	};
} // namespace Durin
