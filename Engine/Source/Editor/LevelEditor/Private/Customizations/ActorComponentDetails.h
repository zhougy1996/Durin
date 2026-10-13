#pragma once

#include "CoreMinimal.h"
#include "PropertyEditor/DetailCustomization.h"
#include "PropertyEditor/DetailLayoutBuilder.h"

namespace Durin { class AActor; class DActorComponent; }

namespace Durin::Editor::Level
{
	struct FLevelEditorContext;
	auto CreateStaticMeshActorDetailsCustomization() -> std::shared_ptr<IDetailCustomization>;

	auto CreateActorComponentDetailsCustomization(
		std::function<DActorComponent*(AActor*)> ResolveComponent) -> std::shared_ptr<IDetailCustomization>;

	auto BuildActorComponentDetails(FLevelEditorContext& Context, AActor* Actor,
		FDetailLayoutBuilder& Builder, std::string_view SearchText) -> void;
}
