#pragma once

#include "CoreMinimal.h"
#include "PropertyEditor/DetailLayoutBuilder.h"

namespace Durin { class DActorComponent; }

namespace Durin::Editor::Level
{
	struct FLevelEditorContext;

	// Provides Level Editor services and per-layout Actor aggregation policy.
	struct FLevelDetailHostContext final : IDetailHostContext
	{
		explicit FLevelDetailHostContext(FLevelEditorContext& InContext) : Context(InContext) {}
		FLevelEditorContext& Context;
		std::optional<std::vector<DActorComponent*>> ComponentDetailsTargets;
	};

	// Adapts a shared layout to the Level Editor's host services.
	class FLevelDetailLayoutBuilder final : public FDetailLayoutBuilder
	{
	public:
		explicit FLevelDetailLayoutBuilder(FLevelEditorContext& Context, std::string_view SearchText = {})
			: FDetailLayoutBuilder(SearchText, &Host), Host(Context) {}
		auto SetComponentDetailsTargets(std::span<DActorComponent* const> Components) -> void
		{
			Host.ComponentDetailsTargets = std::vector<DActorComponent*>(Components.begin(), Components.end());
		}
	private:
		FLevelDetailHostContext Host;
	};

	inline auto GetLevelDetailHostContext(IDetailLayoutBuilder& Builder) -> FLevelDetailHostContext&
	{
		auto* Host = dynamic_cast<FLevelDetailHostContext*>(Builder.GetHostContext());
		requiref(Host, "This detail customization requires Level Editor host services.");
		return *Host;
	}
}
