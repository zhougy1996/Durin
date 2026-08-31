#pragma once

#include "DObject/ObjectPtr.h"
#include "LevelEditorSelection.h"

namespace Durin::Editor
{
	enum class EPlayStartLocation : uint8;
	enum class EPlayDestination : uint8;
}

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DWorld;
	class DLevel;
	class DPackage;
	class FPackagePath;
	using FAssetPath = FPackagePath;
}

namespace Durin::Editor::Level
{
	class FViewportPickingSceneIndex;
	// Shares active world, selection, play, and viewport state across editor panels.
	struct FLevelEditorContext
	{
		DWorld* World = nullptr;
		DLevel* Level = nullptr;
		bool bReadOnly = false;
		bool bSimulatePhysics = true;
		std::function<void(std::string)> ReportError;
		std::function<bool(std::string_view)> RenameLevel;
		std::function<void(AActor*)> FocusActor;
		std::function<void(::Durin::Editor::EPlayStartLocation, ::Durin::Editor::EPlayDestination)> StartPlay;
		std::function<void(bool)> ApplyPlayChanges;
		std::function<bool(std::string_view)> ActivateViewportEditMode;
		std::function<bool(const FAssetPath&, std::string&)> RevealAsset;
		std::function<bool(const FAssetPath&, std::string&)> OpenAsset;

		auto Synchronize(DWorld* CurrentWorld) -> void;
		auto SelectActor(AActor* Actor) -> void;
		auto ToggleActorSelection(AActor* Actor) -> void;
		auto SelectActorRange(AActor* Actor, const std::vector<AActor*>& VisibleActors) -> void;
		auto SetSelectedActors(const std::vector<AActor*>& Actors, AActor* PrimaryActor = nullptr) -> void;
		auto ClearSelection() -> void;
		auto IsActorSelected(const AActor* Actor) const -> bool;
		auto GetPrimarySelectedActor() const -> AActor* { return PrimarySelectedActor.Get(); }
		auto GetSelectedActors() const -> const std::vector<TObjectPtr<AActor>>& { return SelectedActors; }
		auto SelectComponent(DActorComponent* Component) -> void;
		auto GetSelectedComponent() const -> DActorComponent* { return SelectedComponent.Get(); }
		auto SelectSubElement(DActorComponent* Component, const FEditorSubElementSelection& Element) -> void;
		auto ToggleSubElement(DActorComponent* Component, const FEditorSubElementSelection& Element) -> void;
		auto ClearSubElementSelection() -> void { SelectedSubElement = {}; SelectedSubElements.clear(); }
		auto GetSelectedSubElement() const -> const FEditorSubElementSelection& { return SelectedSubElement; }
		auto GetSelectedSubElements() const -> const std::vector<FEditorSubElementSelection>& { return SelectedSubElements; }
		auto IsSubElementSelected(const FEditorSubElementSelection& Element) const -> bool;
		auto SetError(std::string Message) const -> void { if (ReportError) ReportError(std::move(Message)); }
		auto InvalidatePackageSavedState(DPackage* Package = nullptr) const -> void;
		auto GetPickingSceneIndex() const -> const std::shared_ptr<FViewportPickingSceneIndex>& { return PickingSceneIndex; }

	private:
		std::vector<TObjectPtr<AActor>> SelectedActors;
		TObjectPtr<AActor> PrimarySelectedActor;
		TObjectPtr<AActor> SelectionAnchor;
		TObjectPtr<DActorComponent> SelectedComponent;
		FEditorSubElementSelection SelectedSubElement;
		std::vector<FEditorSubElementSelection> SelectedSubElements;
		std::shared_ptr<FViewportPickingSceneIndex> PickingSceneIndex;
	};
} // namespace Durin::Editor::Level
