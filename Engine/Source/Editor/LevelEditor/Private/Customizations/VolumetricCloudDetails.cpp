#include "VolumetricCloudDetails.h"

#include "Components/VolumetricCloudComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "LevelEditorCustomizations.h"
#include "MonaImGui.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto DrawFact(const char* Label, std::string_view Value) -> void
		{
			MonaImGui::PropertyEdit::BeginRow(Label, true);
			ImGui::TextWrapped("%.*s", static_cast<int>(Value.size()), Value.data());
			MonaImGui::PropertyEdit::EndRow(true);
		}

		auto IsEligible(const DVolumetricCloudComponent* Component) -> bool
		{
			return Component && Component->IsRegistered() && Component->IsEnabled()
				&& Component->GetOwner() && !Component->GetOwner()->IsHidden()
				&& Component->GetEligibilityStatus() == "Ready";
		}

		auto IsPreferred(const DVolumetricCloudComponent* A,
			const DVolumetricCloudComponent* B) -> bool
		{
			if (!B) return true;
			if (A->GetPriority() != B->GetPriority())
				return A->GetPriority() > B->GetPriority();
			return std::tuple(A->GetVolumetricCloudSceneId(), A->GetObjectPath(),
				A->GetVolumetricCloudInstanceId())
				< std::tuple(B->GetVolumetricCloudSceneId(), B->GetObjectPath(),
					B->GetVolumetricCloudInstanceId());
		}

		auto FindActive(DLevel* Level) -> DVolumetricCloudComponent*
		{
			DVolumetricCloudComponent* Active = nullptr;
			if (!Level) return nullptr;
			for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
			{
				AActor* Actor = ActorPtr.Get();
				if (!Actor) continue;
				for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetComponents())
				{
					auto* Cloud = Cast<DVolumetricCloudComponent>(ComponentPtr.Get());
					if (IsEligible(Cloud) && IsPreferred(Cloud, Active)) Active = Cloud;
				}
			}
			return Active;
		}

		auto GetCloudStatus(const DVolumetricCloudComponent* Component,
			const DVolumetricCloudComponent* Active) -> std::string
		{
			if (Active == Component) return "Active: selected for rendering.";
			if (IsEligible(Component))
				return "Ignored: another eligible cloud has higher priority.";
			return Component->GetEligibilityStatus();
		}

		class FVolumetricCloudDetailsCustomization final
			: public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext& EditorContext, DObject* Object,
				FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Component = Cast<DVolumetricCloudComponent>(Object);
				if (!Component) return;
				if (FProperty* Eligibility = Component->GetClass()->FindPropertyByName(
					"EligibilityStatus")) Builder.HideProperty(Eligibility);

				Builder.AddCustomRow(
					"Volumetric Cloud Active Ignored Ineligible Eligibility Status",
					[Component, Level = EditorContext.Level](
						::Durin::Editor::FPropertyView&,
						const ::Durin::Editor::FPropertyViewContext&) {
						Component->RefreshEligibilityDiagnostic();
						DVolumetricCloudComponent* Active = FindActive(Level);
						DrawFact("Cloud status", GetCloudStatus(Component, Active));
						return false;
					});
			}
		};
	}

	auto CreateVolumetricCloudDetailsCustomization()
		-> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FVolumetricCloudDetailsCustomization>();
	}
}
