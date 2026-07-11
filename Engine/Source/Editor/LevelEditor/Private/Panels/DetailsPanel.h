#pragma once

#include "DObject/ObjectPtr.h"
#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DObject;
	class FProperty;

	class FDetailsPanel final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "Details"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		auto DrawTransform(AActor* Actor) -> void;
		auto DrawComponents(FLevelEditorContext& Context, AActor* Actor) -> void;
		auto DrawReflectedProperties(FLevelEditorContext& Context, DObject* Object) -> void;
		auto DrawProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex) -> void;
		auto AssignObjectProperty(FLevelEditorContext& Context, DObject* Object, FProperty* Property, uint32 ArrayIndex, DObject* Value) -> bool;

		std::array<char, 128> ComponentTypeSearchText{};
		TObjectPtr<DActorComponent> PendingRemoveComponent;
		TObjectPtr<AActor> PropertyActor;
		TObjectPtr<DActorComponent> SelectedComponent;
		std::array<char, 256> AssetSearchText{};
	};
} // namespace Durin
