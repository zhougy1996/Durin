#include "VolumetricCloudDetails.h"

#include "Components/VolumetricCloudComponent.h"
#include "DObject/Package.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "LevelEditorCustomizations.h"
#include "MonaImGui.h"
#include "Texture/Texture2D.h"
#include "Texture/VolumeTexture.h"
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

		auto RevealButton(const char* Label, DObject* Asset,
			const ::Durin::Editor::FPropertyViewContext& Context) -> void
		{
			FAssetPath Path;
			const bool bAvailable = Asset && Asset->GetPackage()
				&& FAssetPath::TryCreate(Asset->GetPackage()->GetPackagePath(), Path)
				&& static_cast<bool>(Context.RevealAsset);
			if (!bAvailable) ImGui::BeginDisabled();
			if (ImGui::SmallButton(Label))
			{
				std::string Error;
				if (!Context.RevealAsset(Path, Error) && Context.ReportError)
					Context.ReportError(std::move(Error));
			}
			if (!bAvailable) ImGui::EndDisabled();
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
					"Volumetric Cloud Active Ignored Eligibility Status Priority Inputs Roles",
					[Component, Level = EditorContext.Level](
						::Durin::Editor::FPropertyView&,
						const ::Durin::Editor::FPropertyViewContext& Context) {
						Component->RefreshEligibilityDiagnostic();
						DVolumetricCloudComponent* Active = FindActive(Level);
						DrawFact("Scene selection", Active == Component ? "Active"
							: IsEligible(Component) ? "Ignored by a higher-priority cloud"
							: "Ineligible");
						DrawFact("Eligibility", Component->GetEligibilityStatus());
						DrawFact("Base density", "Required low-frequency cloud shape");
						DrawFact("Detail density", "Required high-frequency erosion");
						DrawFact("Weather", "Optional two-dimensional coverage control");
						MonaImGui::PropertyEdit::BeginRow("Asset actions", true);
						RevealButton("Reveal Base", Component->GetBaseDensityTexture(), Context);
						ImGui::SameLine();
						RevealButton("Reveal Detail", Component->GetDetailDensityTexture(), Context);
						ImGui::SameLine();
						RevealButton("Reveal Weather", Component->GetWeatherTexture(), Context);
						MonaImGui::PropertyEdit::EndRow(true);
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
