#include "LevelEditorContext.h"

#include "Engine/World.h"
#include "Engine/Level.h"

namespace Durin
{
	auto FLevelEditorContext::Synchronize(DWorld* CurrentWorld) -> void
	{
		DLevel* CurrentLevel = CurrentWorld ? CurrentWorld->GetCurrentLevel() : nullptr;
		if (World != CurrentWorld || Level != CurrentLevel)
		{
			World = CurrentWorld;
			Level = CurrentLevel;
			ClearSelection();
			return;
		}

		std::erase_if(SelectedActors, [this](const TObjectPtr<AActor>& Actor) { return !Actor || World == nullptr || !World->ContainsActor(Actor.Get()); });
		if (!SelectedActors.empty() && !IsActorSelected(PrimarySelectedActor.Get()))
		{
			PrimarySelectedActor = SelectedActors.empty() ? nullptr : SelectedActors.back();
		}
		else if (SelectedActors.empty()) PrimarySelectedActor = nullptr;
		if (SelectionAnchor && (World == nullptr || !World->ContainsActor(SelectionAnchor.Get()))) SelectionAnchor = nullptr;
	}

	auto FLevelEditorContext::SelectActor(AActor* Actor) -> void
	{
		ClearSelection();
		if (World != nullptr && World->ContainsActor(Actor))
		{
			SelectedActors.emplace_back(Actor);
			PrimarySelectedActor = Actor;
			SelectionAnchor = Actor;
		}
	}

	auto FLevelEditorContext::ToggleActorSelection(AActor* Actor) -> void
	{
		if (World == nullptr || !World->ContainsActor(Actor)) return;
		const auto It = std::ranges::find_if(SelectedActors, [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
		if (It == SelectedActors.end())
		{
			SelectedActors.emplace_back(Actor);
			PrimarySelectedActor = Actor;
		}
		else
		{
			SelectedActors.erase(It);
			if (PrimarySelectedActor.Get() == Actor) PrimarySelectedActor = SelectedActors.empty() ? nullptr : SelectedActors.back();
		}
		SelectionAnchor = Actor;
	}

	auto FLevelEditorContext::SelectActorRange(AActor* Actor, const std::vector<AActor*>& VisibleActors) -> void
	{
		if (World == nullptr || !World->ContainsActor(Actor)) return;
		AActor* Anchor = SelectionAnchor.Get();
		auto First = std::ranges::find(VisibleActors, Anchor);
		auto Last = std::ranges::find(VisibleActors, Actor);
		if (First == VisibleActors.end() || Last == VisibleActors.end())
		{
			SelectActor(Actor);
			return;
		}
		if (First > Last) std::swap(First, Last);
		std::vector<AActor*> Range(First, Last + 1);
		SetSelectedActors(Range, Actor);
	}

	auto FLevelEditorContext::SetSelectedActors(const std::vector<AActor*>& Actors, AActor* PrimaryActor) -> void
	{
		SelectedActors.clear();
		for (AActor* Actor : Actors)
		{
			if (World && World->ContainsActor(Actor) && !IsActorSelected(Actor)) SelectedActors.emplace_back(Actor);
		}
		PrimarySelectedActor = IsActorSelected(PrimaryActor) ? PrimaryActor : (SelectedActors.empty() ? nullptr : SelectedActors.back().Get());
		SelectionAnchor = PrimarySelectedActor;
	}

	auto FLevelEditorContext::ClearSelection() -> void
	{
		SelectedActors.clear();
		PrimarySelectedActor = nullptr;
		SelectionAnchor = nullptr;
	}

	auto FLevelEditorContext::IsActorSelected(const AActor* Actor) const -> bool
	{
		return Actor && std::ranges::any_of(SelectedActors, [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
	}
} // namespace Durin
