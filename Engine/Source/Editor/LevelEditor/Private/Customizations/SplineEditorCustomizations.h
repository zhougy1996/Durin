#pragma once

#include "LevelEditorCustomizations.h"
#include "LevelEditorViewportEditing.h"

namespace Durin
{
	class DSplineComponent;
}

namespace Durin::Editor::Level
{
	auto CreateSplineComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>;
	auto CreateSplineDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
	auto CreateSplineMeshActorDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
	auto RegisterSplineViewportEditMode(
		FModuleOwnedCallbackGate OwnerGate = {}) -> FLevelViewportEditModeHandle;
	auto CalculateSplineAppendPosition(const DSplineComponent& Spline) -> FVector3;
	auto SplitSplineSegment(DSplineComponent& Spline, uint32 SegmentIndex, double T, FGuid* OutPointId = nullptr) -> bool;
} // namespace Durin::Editor::Level
