#pragma once

#include "CoreMinimal.h"

#include "LevelEditorAPI.h"
#include "LevelEditorSelection.h"
#include "DObject/WeakObjectPtr.h"
#include "Rendering/PrimitiveComponentId.h"
#include "SceneView.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DLevel;
}

namespace Durin::Editor::Level
{

	// Selects the semantic families considered by one viewport request.
	enum class EViewportPickLayer : uint8
	{
		None = 0,
		SceneGeometry = 1 << 0,
		EditorVisualization = 1 << 1
	};
	ENUM_CLASS_FLAGS(EViewportPickLayer);

	// Distinguishes a completed blank-space query from invalid input or rendering failure.
	enum class EViewportPickStatus : uint8
	{
		Invalid,
		Completed,
		Failed
	};

	// Identifies the semantic source of a winning hit.
	enum class EViewportPickHitKind : uint8
	{
		SceneGeometry,
		EditorVisualization
	};

	// Captures one immutable view-space picking query in output-target pixel coordinates.
	struct FViewportPickRequest
	{
		TWeakObjectPtr<DLevel> Level;
		FSceneView View;
		FVector2f ViewportPosition{0.0f};
		EViewportPickLayer Layers = EViewportPickLayer::SceneGeometry | EViewportPickLayer::EditorVisualization;
	};

	// Carries a validated semantic hit without transferring selection authority.
	struct FViewportPickHit
	{
		EViewportPickHitKind Kind = EViewportPickHitKind::SceneGeometry;
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DActorComponent> Component;
		FEditorSubElementSelection Element;
		double Distance = std::numeric_limits<double>::max();
	};

	// Reports one request outcome; Completed with no Hit is a successful blank-space query.
	struct FViewportPickResult
	{
		EViewportPickStatus Status = EViewportPickStatus::Invalid;
		std::optional<FViewportPickHit> Hit;
	};

} // namespace Durin::Editor::Level
