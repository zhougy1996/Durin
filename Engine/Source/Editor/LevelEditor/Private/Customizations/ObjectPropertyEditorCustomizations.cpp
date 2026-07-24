#include "Customizations/ObjectPropertyEditorCustomizations.h"

#include "Components/SceneComponent.h"
#include "Engine/Actor.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin
{
	namespace
	{
		// Adds actor-specific rows and hides redundant reflected properties.
		class FActorDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext&, DObject* Object,
				FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Actor = Cast<AActor>(Object);
				DSceneComponent* RootComponent = Actor ? Actor->GetRootComponent() : nullptr;
				FProperty* TransformProperty = RootComponent
					? RootComponent->GetClass()->FindPropertyByName("RelativeTransform") : nullptr;
				if (TransformProperty)
				{
					Builder.AddProperty(RootComponent, TransformProperty, 0, {.Label = "Transform"},
						"Location Rotation Scale");
				}
			}
		};
	} // namespace

	auto CreateActorDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FActorDetailsCustomization>();
	}
}
