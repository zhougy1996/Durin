#include "LevelDetailCustomization.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"
#include "Customizations/ActorComponentDetails.h"

#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Actors/StaticMeshActor.h"
#include "StaticMeshMaterialSlotDetails.h"
#include "DObject/Class.h"
#include "DObject/Property.h"
#include "Engine/Actor.h"
#include "LevelEditorCustomizations.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiPropertyTable.h"

namespace Durin::Editor::Level
{
	namespace
	{
		// Presents a StaticMeshActor's primary component through task-oriented categories.
		class FStaticMeshActorDetailsCustomization final : public IDetailCustomization
		{
		public:
			auto CustomizeDetails(IDetailLayoutBuilder& Builder) -> void override
			{
				auto* Actor = Cast<AStaticMeshActor>(Builder.GetObject());
				if (!Actor) return;
				GetLevelDetailHostContext(Builder).ComponentDetailsTargets = std::vector<DActorComponent*>{};
				auto* Component = Actor->GetStaticMeshComponent();
				if (!Component) return;
				const std::string Source = std::format("{} {}", Component->GetName(), Component->GetClass()->GetQualifiedName());
				auto& Mesh = Builder.EditCategory("StaticMesh", "Static Mesh");
				Mesh.SetSortOrder(-90);
				Mesh.SetTooltip(Source);
				Mesh.AddProperty(Component, Component->GetClass()->FindPropertyByName("StaticMesh"), 0, {}, Source);
				AddStaticMeshMaterialSlotDetails(Builder, Component, "StaticMesh");
				auto& Rendering = Builder.EditCategory("Rendering");
				Rendering.SetSortOrder(-80);
				Rendering.SetTooltip(Source);
				Rendering.AddProperty(Component, Component->GetClass()->FindPropertyByName("bVisible"), 0, {}, Source);
				auto& Collision = Builder.EditCategory("Collision");
				Collision.SetSortOrder(-70);
				Collision.SetTooltip(Source);
				Collision.AddProperty(Component, Component->GetClass()->FindPropertyByName("BodyInstance"), 0, {}, Source);
			}
		};
		// Selects the component that supplies a built-in Actor's primary controls.
		class FActorComponentDetailsCustomization final : public IDetailCustomization
		{
		public:
			explicit FActorComponentDetailsCustomization(std::function<DActorComponent*(AActor*)> InResolve)
				: Resolve(std::move(InResolve)) {}
			auto CustomizeDetails(IDetailLayoutBuilder& Builder) -> void override
			{
				DObject* Object = Builder.GetObject();
				if (auto* Actor = Cast<AActor>(Object))
				{
					const std::array Targets{Resolve(Actor)};
					GetLevelDetailHostContext(Builder).ComponentDetailsTargets = std::vector<DActorComponent*>(Targets.begin(), Targets.end());
				}
			}
		private:
			std::function<DActorComponent*(AActor*)> Resolve;
		};
	}

	auto CreateActorComponentDetailsCustomization(std::function<DActorComponent*(AActor*)> ResolveComponent)
		-> std::shared_ptr<IDetailCustomization>
	{
		return std::make_shared<FActorComponentDetailsCustomization>(std::move(ResolveComponent));
	}

	auto CreateStaticMeshActorDetailsCustomization() -> std::shared_ptr<IDetailCustomization>
	{
		return std::make_shared<FStaticMeshActorDetailsCustomization>();
	}

	auto BuildActorComponentDetails(FLevelEditorContext& Context, AActor* Actor,
		FDetailLayoutBuilder& Builder, std::string_view SearchText) -> void
	{
		if (!Actor) return;
		DSceneComponent* Root = Actor->GetRootComponent();
		if (Root && Root->GetCreationMethod() != EComponentCreationMethod::Generated)
		{
			auto& TransformCategory = Builder.EditCategory("Transform");
			TransformCategory.SetSortOrder(-100);
			TransformCategory.SetTooltip(std::format("{} {}", Root->GetName(), Root->GetClass()->GetQualifiedName()));
			TransformCategory.AddProperty(Root, Root->GetClass()->FindPropertyByName("RelativeTransform"),
				0, {.Label = "Transform", .bShowTransformHeader = false}, "Location Rotation Scale");
		}

		std::vector<DActorComponent*> Components;
		auto* Host = dynamic_cast<FLevelDetailHostContext*>(Builder.GetHostContext());
		if (Host && Host->ComponentDetailsTargets) Components = *Host->ComponentDetailsTargets;
		else for (const auto& Entry : Actor->GetComponents()) Components.push_back(Entry.Get());
		std::unordered_set<DActorComponent*> Visited;
		for (DActorComponent* Component : Components)
		{
			if (!Component || !Actor->OwnsComponent(Component) || !Visited.insert(Component).second
				|| Component->GetCreationMethod() == EComponentCreationMethod::Generated) continue;
			const std::string Label = Component->GetName();
			const std::string TypeName = Component->GetClass()->GetQualifiedName().ToString();
			const bool bMatchesComponent = !SearchText.empty() && StringUtils::ContainsInsensitive(Label + " " + TypeName, SearchText);
			auto ComponentBuilder = std::make_shared<FLevelDetailLayoutBuilder>(Context, bMatchesComponent ? std::string_view{} : SearchText);
			ComponentBuilder->ApplyRegisteredCustomizations(Component);
			if (!ComponentBuilder->IsReplacingDefaultProperties())
			{
				Component->GetClass()->ForEachProperty([&](FProperty* Property) {
					if (!Property || !Property->HasAnyPropertyFlags(EPropertyFlags::Edit)
						|| Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
					if (Component == Root && Property->NamePrivate == "RelativeTransform") return;
					for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
						if (ComponentBuilder->ShouldDrawDefaultProperty(Component, *Property, Index))
							ComponentBuilder->AddProperty(Component, Property, Index, {}, Property->GetTypedMetadata().Category);
				});
			}
			if (ComponentBuilder->GetVisibleRowCount() == 0) continue;
			Builder.AddCustomRow(SearchText, [ComponentBuilder, Component, Label, TypeName, bSearching = !SearchText.empty()](
				::Durin::Editor::FPropertyView& View, const ::Durin::Editor::FPropertyViewContext& ViewContext) {
				ImGui::PushID(Component);
				if (bSearching) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
				const bool bOpen = MonaImGui::PropertyEdit::BeginGroup("##ComponentProperties", Label.c_str());
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TypeName.c_str());
				bool bChanged = false;
				if (bOpen)
				{
					bChanged = ComponentBuilder->DrawRows(View, ViewContext).bChanged;
					MonaImGui::PropertyEdit::EndGroup();
				}
				ImGui::PopID();
				return bChanged;
			});
		}
	}
}
