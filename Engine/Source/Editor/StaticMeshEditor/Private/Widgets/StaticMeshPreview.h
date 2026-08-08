#pragma once

#include "Math/Box.h"
#include "StaticMeshEditorAPI.h"

namespace Durin
{
	class DStaticMesh;

	// Pure camera state used by the StaticMesh preview viewport and its input tests.
	class STATICMESHEDITOR_API FStaticMeshPreviewController final
	{
	public:
		auto FrameBounds(const FBox& Bounds) -> void;
		auto Orbit(float DeltaX, float DeltaY) -> void;
		auto Pan(float DeltaX, float DeltaY) -> void;
		auto Zoom(float WheelDelta) -> void;
		auto Reset() -> void;
		auto GetTarget() const -> const FVector3& { return Target; }
		auto GetDistance() const -> double { return Distance; }
		auto GetYawDegrees() const -> double { return YawDegrees; }
		auto GetPitchDegrees() const -> double { return PitchDegrees; }

	private:
		FBox FramedBounds;
		FVector3 Target{0.0};
		double Distance = 4.0;
		double YawDegrees = -45.0;
		double PitchDegrees = 25.0;
	};

	// Owns one isolated world, mesh component, light, viewport, and camera.
	class STATICMESHEDITOR_API FStaticMeshPreview final
	{
	public:
		explicit FStaticMeshPreview(uint64 PreviewId);
		~FStaticMeshPreview();

		auto SetVisible(bool bVisible) -> void;
		auto Draw(DStaticMesh* Mesh, uint64 Revision, float PanelHeight = 0.0f) -> void;
		auto ResetView() -> void;
		auto SetWireframe(bool bWireframe) -> void;
		auto IsWireframe() const -> bool;

	private:
		class FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
