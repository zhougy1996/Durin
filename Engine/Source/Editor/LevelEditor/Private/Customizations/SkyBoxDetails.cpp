#include "SkyBoxDetails.h"

#include "Components/SkyBoxComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "LevelEditorCustomizations.h"
#include "MonaImGui.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin
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
				Builder.AddCustomRow("Sky Box Conflict Active Ignored",
					[Level = Context.Level](Editor::FPropertyView&, const Editor::FPropertyViewContext&) {
						const FSkyBoxConflictModel Model(Level);
						if (!Model.HasConflict()) return false;

						MonaImGui::PropertyEdit::BeginRow("Sky Box Conflict", true);
						const FSkyBoxConflictEntry* Active = Model.GetActive();
						ImGui::PushStyleColor(ImGuiCol_Text,
							MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning));
						ImGui::TextWrapped("Multiple visible sky boxes are registered. Active: %s",
							Active ? Active->ActorName.c_str() : "<none>");
						ImGui::PopStyleColor();
						for (const FSkyBoxConflictEntry& Entry : Model.GetEntries())
						{
							if (Entry.bActive) continue;
							ImGui::TextDisabled("Ignored: %s (%s)",
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
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
			{
				auto* Component = Cast<DSkyBoxComponent>(ComponentPtr.Get());
				if (Component && Component->IsRegistered()) Candidates.push_back({Component, Actor});
			}
		}
		std::ranges::sort(Candidates, [](const FCandidate& A, const FCandidate& B) {
			return std::tuple(A.Component->GetSkyBoxSceneId(), A.Component->GetObjectPath(),
				A.Component->GetSkyBoxInstanceId())
				< std::tuple(B.Component->GetSkyBoxSceneId(), B.Component->GetObjectPath(),
					B.Component->GetSkyBoxInstanceId());
		});
		Entries.reserve(Candidates.size());
		for (size_t Index = 0; Index < Candidates.size(); ++Index)
		{
			Entries.push_back({
				.Component = Candidates[Index].Component,
				.ActorName = Candidates[Index].Actor->GetName(),
				.ObjectPath = Candidates[Index].Component->GetObjectPath(),
				.bActive = Index == 0,
			});
		}
	}

	auto CreateSkyBoxDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FSkyBoxDetailsCustomization>();
	}
}
