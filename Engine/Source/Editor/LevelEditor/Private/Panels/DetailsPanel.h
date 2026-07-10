#pragma once

#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	class AActor;
	class FProperty;

	class FDetailsPanel final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "Details"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		auto DrawTransform(AActor* Actor) -> void;
		auto DrawComponents(AActor* Actor) -> void;
		auto DrawReflectedProperties(AActor* Actor) -> void;
		auto DrawProperty(AActor* Actor, FProperty* Property, uint32 ArrayIndex) -> void;
	};
} // namespace Durin
