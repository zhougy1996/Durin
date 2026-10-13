#include "LevelDetailCustomization.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"
#include "Customizations/ActorComponentDetails.h"

#include "Components/ActorComponent.h"
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

	auto BuildActorComponentDetails(FLevelEditorContext& Context, AActor* Actor,
		FDetailLayoutBuilder& Builder, std::string_view SearchText) -> void
	{
		if (!Actor) return;
		DSceneComponent* Root = Actor->GetRootComponent();
		if (Root && Root->GetCreationMethod() != EComponentCreationMethod::Generated)
		{
			auto& TransformCategory = Builder.EditCategory("Transform");
			TransformCategory.SetSortOrder(-100);
			TransformCategory.AddProperty(Root, Root->GetClass()->FindPropertyByName("RelativeTransform"),
				0, {.Label = "Transform"}, "Location Rotation Scale");
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
			const std::string Label = std::format("{} ({})", Component->GetName(), Component->GetClass()->GetName());
			const bool bMatchesComponent = !SearchText.empty() && StringUtils::ContainsInsensitive(Label, SearchText);
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
			Builder.AddCustomRow(SearchText, [ComponentBuilder, Component, Label, bSearching = !SearchText.empty()](
				::Durin::Editor::FPropertyView& View, const ::Durin::Editor::FPropertyViewContext& ViewContext) {
				ImGui::PushID(Component);
				MonaImGui::PropertyEdit::BeginRow("Component", ViewContext.bReadOnly);
				if (bSearching) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
				const bool bOpen = ImGui::TreeNodeEx("##ComponentProperties", ImGuiTreeNodeFlags_DefaultOpen, "%s", Label.c_str());
				MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
				bool bChanged = false;
				if (bOpen)
				{
					bChanged = ComponentBuilder->DrawRows(View, ViewContext).bChanged;
					ImGui::TreePop();
				}
				ImGui::PopID();
				return bChanged;
			});
		}
	}
}
