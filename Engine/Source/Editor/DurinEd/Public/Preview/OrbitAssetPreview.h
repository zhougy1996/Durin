#pragma once

#include "DurinEdAPI.h"
#include "Math/Box.h"
#include "Preview/AssetPreviewHost.h"

namespace Durin::Editor
{
	// Deterministic camera state shared by previews that orbit an asset's bounds.
	class FOrbitAssetPreviewController final
	{
	public:
		DURINED_API auto FrameBounds(const FBox& Bounds) -> void;
		DURINED_API auto Orbit(float DeltaX, float DeltaY) -> void;
		DURINED_API auto Pan(float DeltaX, float DeltaY) -> void;
		DURINED_API auto Zoom(float WheelDelta) -> void;
		DURINED_API auto Reset() -> void;
		DURINED_API auto ApplyInput(const FAssetPreviewViewportInput& Input) -> void;

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

	// Standard reversed-Z perspective viewport for orbiting bounded assets.
	class FOrbitAssetPreviewViewportClient final : public FAssetPreviewViewportClient
	{
	public:
		DURINED_API auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override;
		auto GetController() -> FOrbitAssetPreviewController& { return Controller; }
		auto GetController() const -> const FOrbitAssetPreviewController& { return Controller; }

		DURINED_API auto SetWireframe(bool bWireframe) -> void;
		DURINED_API auto IsWireframe() const -> bool;
		DURINED_API auto SetLit(bool bLit) -> void;
		DURINED_API auto IsLit() const -> bool;

	private:
		FOrbitAssetPreviewController Controller;
	};
}
