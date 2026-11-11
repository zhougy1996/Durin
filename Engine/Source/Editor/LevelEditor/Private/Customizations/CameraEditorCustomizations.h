#pragma once

#include "CoreMinimal.h"
#include "PropertyEditor/DetailCustomization.h"

#include "LevelEditorCustomizations.h"

namespace Durin::LevelEditor
{
	auto CreateCameraComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>;
	auto CreateCameraDetailsCustomization() -> std::shared_ptr<IDetailCustomization>;
}
