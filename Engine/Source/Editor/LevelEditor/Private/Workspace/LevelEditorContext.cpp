#include "Workspace/LevelEditorContext.h"

#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transaction.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Components/ActorComponent.h"
#include "Engine/Actor.h"
#include "Viewport/ViewportPickingSceneIndex.h"

namespace Durin::Editor::Level
{
	auto FLevelEditorContext::Synchronize(DWorld* CurrentWorld) -> void
	{
		if (!PickingSceneIndex) PickingSceneIndex = std::make_shared<FViewportPickingSceneIndex>();
		DLevel* CurrentLevel = CurrentWorld ? CurrentWorld->GetCurrentLevel() : nullptr;
		if (World != CurrentWorld || Level != CurrentLevel)
		{
			World = CurrentWorld;
			Level = CurrentLevel;
			PickingSceneIndex->SetLevel(CurrentLevel);
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
		AActor* Primary = PrimarySelectedActor.Get();
		if (!Primary || !SelectedComponent || !Primary->OwnsComponent(SelectedComponent.Get()))
		{
			SelectedComponent = nullptr;
			SelectedSubElement = {};
			SelectedSubElements.clear();
		}
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
		if (!PrimarySelectedActor || !SelectedComponent || !PrimarySelectedActor->OwnsComponent(SelectedComponent.Get()))
		{
			SelectedComponent = nullptr;
			SelectedSubElement = {};
			SelectedSubElements.clear();
		}
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
		if (!PrimarySelectedActor || !SelectedComponent || !PrimarySelectedActor->OwnsComponent(SelectedComponent.Get()))
		{
			SelectedComponent = nullptr;
			SelectedSubElement = {};
			SelectedSubElements.clear();
		}
	}

	auto FLevelEditorContext::ClearSelection() -> void
	{
		SelectedActors.clear();
		PrimarySelectedActor = nullptr;
		SelectionAnchor = nullptr;
		SelectedComponent = nullptr;
		SelectedSubElement = {};
		SelectedSubElements.clear();
	}

	auto FLevelEditorContext::SelectComponent(DActorComponent* Component) -> void
	{
		AActor* Primary = PrimarySelectedActor.Get();
		DActorComponent* Resolved = Primary && Component && Primary->OwnsComponent(Component) ? Component : nullptr;
		if (SelectedComponent.Get() != Resolved) { SelectedSubElement = {}; SelectedSubElements.clear(); }
		SelectedComponent = Resolved;
	}

	auto FLevelEditorContext::SelectSubElement(DActorComponent* Component, const FEditorSubElementSelection& Element) -> void
	{
		SelectComponent(Component);
		if (SelectedComponent && Element.IsValid())
		{
			SelectedSubElement = Element;
			SelectedSubElements = {Element};
		}
	}

	auto FLevelEditorContext::ToggleSubElement(DActorComponent* Component, const FEditorSubElementSelection& Element) -> void
	{
		SelectComponent(Component);
		if (!SelectedComponent || !Element.IsValid()) return;
		const auto It = std::ranges::find(SelectedSubElements, Element);
		if (It == SelectedSubElements.end())
		{
			SelectedSubElements.push_back(Element);
			SelectedSubElement = Element;
		}
		else
		{
			SelectedSubElements.erase(It);
			SelectedSubElement = SelectedSubElements.empty() ? FEditorSubElementSelection{} : SelectedSubElements.back();
		}
	}

	auto FLevelEditorContext::IsSubElementSelected(const FEditorSubElementSelection& Element) const -> bool
	{
		return std::ranges::find(SelectedSubElements, Element) != SelectedSubElements.end();
	}

	auto FLevelEditorContext::IsActorSelected(const AActor* Actor) const -> bool
	{
		return Actor && std::ranges::any_of(SelectedActors, [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
	}

	auto FLevelEditorContext::InvalidatePackageSavedState(DPackage* Package) const -> void
	{
		if (!Package && Level) Package = Level->GetPackage();
		if (!Package || !Package->IsAssetPackage()) return;
		if (GEditor)
			GEditor->GetTransactionManager().InvalidateSavedState(*Package);
		else
			Package->MarkDirty();
	}
} // namespace Durin::Editor::Level
