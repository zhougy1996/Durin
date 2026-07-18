#pragma once

#include "DObject/ObjectPtr.h"

namespace Durin
{
	enum class EEditorPlayStartLocation : uint8;
	enum class EEditorPlayDestination : uint8;
	class AActor;
	class DWorld;
	class DLevel;

	struct FLevelEditorContext
	{
		DWorld* World = nullptr;
		DLevel* Level = nullptr;
		bool bReadOnly = false;
		bool bSimulatePhysics = true;
		std::function<void(std::string)> ReportError;
		std::function<bool(std::string_view)> RenameLevel;
		std::function<void(EEditorPlayStartLocation, EEditorPlayDestination)> StartPlay;
		std::function<void(bool)> ApplyPlayChanges;

		auto Synchronize(DWorld* CurrentWorld) -> void;
		auto SelectActor(AActor* Actor) -> void;
		auto ToggleActorSelection(AActor* Actor) -> void;
		auto SelectActorRange(AActor* Actor, const std::vector<AActor*>& VisibleActors) -> void;
		auto SetSelectedActors(const std::vector<AActor*>& Actors, AActor* PrimaryActor = nullptr) -> void;
		auto ClearSelection() -> void;
		auto IsActorSelected(const AActor* Actor) const -> bool;
		auto GetPrimarySelectedActor() const -> AActor* { return PrimarySelectedActor.Get(); }
		auto GetSelectedActors() const -> const std::vector<TObjectPtr<AActor>>& { return SelectedActors; }
		auto SetError(std::string Message) const -> void { if (ReportError) ReportError(std::move(Message)); }

	private:
		std::vector<TObjectPtr<AActor>> SelectedActors;
		TObjectPtr<AActor> PrimarySelectedActor;
		TObjectPtr<AActor> SelectionAnchor;
	};
} // namespace Durin
