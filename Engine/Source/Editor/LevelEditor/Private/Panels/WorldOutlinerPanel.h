#pragma once

#include "DObject/ObjectPtr.h"
#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	class AActor;
	class FWorldOutlinerPanel final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "World Outliner"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

	private:
		std::array<char, 128> SearchText{};
		std::array<char, 128> ActorTypeSearchText{};
		TObjectPtr<AActor> PendingDeleteActor;
	};
} // namespace Durin
