#pragma once

#include "LevelEditorAPI.h"
#include "LevelEditorSelection.h"
#include "DObject/WeakObjectPtr.h"
#include "IScene.h"
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

	// Describes the geometric precision required by a request.
	enum class EViewportPickPrecision : uint8
	{
		ActorComponentSurface
	};

	// Separates independently superseded request streams.
	enum class EViewportPickPurpose : uint8
	{
		ClickSelection
	};

	// Reports the lifecycle state of one ticket without conflating empty space with failure.
	enum class EViewportPickStatus : uint8
	{
		Invalid,
		Pending,
		Completed,
		Cancelled,
		Invalidated,
		Failed
	};

	// Identifies the semantic source of a winning hit.
	enum class EViewportPickHitKind : uint8
	{
		SceneGeometry,
		EditorVisualization
	};

	// Opaque per-viewport identity for polling, cancellation, and supersession.
	struct FViewportPickTicket
	{
		uint64 Id = 0;
		explicit operator bool() const { return Id != 0; }
		auto operator<=>(const FViewportPickTicket&) const = default;
	};

	// Captures one immutable view-space picking query in output-target pixel coordinates.
	struct FViewportPickRequest
	{
		uint64 RequestId = 0;
		uint64 ViewportGeneration = 0;
		TWeakObjectPtr<DLevel> Level;
		FSceneView View;
		FVector2f ViewportPosition{0.0f};
		EViewportPickLayer Layers = EViewportPickLayer::SceneGeometry | EViewportPickLayer::EditorVisualization;
		EViewportPickPrecision Precision = EViewportPickPrecision::ActorComponentSurface;
		EViewportPickPurpose Purpose = EViewportPickPurpose::ClickSelection;
	};

	// Carries a validated semantic hit without transferring selection authority.
	struct FViewportPickHit
	{
		EViewportPickHitKind Kind = EViewportPickHitKind::SceneGeometry;
		FPrimitiveSceneId PrimitiveId = InvalidPrimitiveSceneId;
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DActorComponent> Component;
		FEditorSubElementSelection Element;
		double Distance = std::numeric_limits<double>::max();
		int32 Priority = std::numeric_limits<int32>::min();
		uint64 StableTieKey = 0;
		bool bDepthIndependent = false;
	};

	// Reports one request outcome; Completed with no Hit is a successful blank-space query.
	struct FViewportPickCompletion
	{
		EViewportPickStatus Status = EViewportPickStatus::Invalid;
		std::optional<FViewportPickHit> Hit;
	};

	// Returns the ticket and any completion available synchronously at submission.
	struct FViewportPickSubmission
	{
		FViewportPickTicket Ticket;
		FViewportPickCompletion Completion;
	};

	// Applies the lasting cross-family ordering contract to two already valid candidates.
	LEVELEDITOR_API auto IsViewportPickHitPreferred(const FViewportPickHit& Candidate, const FViewportPickHit& Current) -> bool;
} // namespace Durin::Editor::Level