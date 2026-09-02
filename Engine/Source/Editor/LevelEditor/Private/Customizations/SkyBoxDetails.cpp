#include "SkyBoxDetails.h"

#include "Components/SkyBoxComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "LevelEditorCustomizations.h"
#include "MonaImGui.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin::Editor::Level
{
	namespace
	{
		class FSkyBoxDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext& Context, DObject* Object,
				FObjectPropertyViewBuilder& Builder) -> void override
			{
				if (!Cast<DSkyBoxComponent>(Object) || !Context.Level) return;
				Builder.AddCustomRow("Sky Box Conflict",
					[Level = Context.Level](::Durin::Editor::FPropertyView&, const ::Durin::Editor::FPropertyViewContext&) {
						const FSkyBoxConflictModel Model(Level);
						if (!Model.HasConflict()) return false;

						MonaImGui::PropertyEdit::BeginRow("Sky Box Conflict", true);
						ImGui::PushStyleColor(ImGuiCol_Text,
							MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning));
						ImGui::TextWrapped("Multiple visible sky boxes are registered; a scene accepts only one.");
						ImGui::PopStyleColor();
						for (const FSkyBoxConflictEntry& Entry : Model.GetEntries())
						{
							ImGui::TextDisabled("Conflicting: %s (%s)",
								Entry.ActorName.c_str(), Entry.ObjectPath.c_str());
						}
						MonaImGui::PropertyEdit::EndRow(true);
						return false;
					});
			}
		};
	}

	FSkyBoxConflictModel::FSkyBoxConflictModel(DLevel* Level)
	{
		if (!Level) return;
		struct FCandidate
		{
			DSkyBoxComponent* Component = nullptr;
			AActor* Actor = nullptr;
		};
		std::vector<FCandidate> Candidates;
		for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (!Actor || Actor->IsHidden()) continue;
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetComponents())
			{
				auto* Component = Cast<DSkyBoxComponent>(ComponentPtr.Get());
				if (Component && Component->IsRegistered()) Candidates.push_back({Component, Actor});
			}
		}
		Entries.reserve(Candidates.size());
		for (size_t Index = 0; Index < Candidates.size(); ++Index)
		{
			Entries.push_back({
				.Component = Candidates[Index].Component,
				.ActorName = Candidates[Index].Actor->GetName(),
				.ObjectPath = Candidates[Index].Component->GetObjectPath(),
			});
		}
	}

	auto CreateSkyBoxDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FSkyBoxDetailsCustomization>();
	}
}
