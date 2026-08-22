#include "Preview/OrbitAssetPreview.h"

#include "Math/Operations.h"
#include "SceneView.h"
#include "SceneViewProjection.h"

namespace Durin::Editor
{
	namespace
	{
		constexpr double FieldOfViewDegrees = 42.0;
		constexpr double MinimumDistance = 0.05;
		constexpr double MaximumDistance = 1000000.0;
		constexpr double RotationSensitivity = 0.25;
		constexpr double PanSensitivity = 0.0015;
		constexpr double ZoomScale = 0.85;
	}

	auto FOrbitAssetPreviewController::FrameBounds(const FBox& Bounds) -> void
	{
		const FVector3 Extent = Bounds.GetExtent();
		const double MaxExtent = std::max({Extent.x, Extent.y, Extent.z});
		if (!Bounds.bIsValid || !std::isfinite(MaxExtent) || MaxExtent <= 0.0) return;
		FramedBounds = Bounds;
		Target = Bounds.GetCenter();
		Distance = std::clamp(MaxExtent * 1.35
			/ std::tan(Math::DegreesToRadians(FieldOfViewDegrees * 0.5)),
			MinimumDistance, MaximumDistance);
		YawDegrees = -45.0;
		PitchDegrees = 25.0;
	}

	auto FOrbitAssetPreviewController::Orbit(float DeltaX, float DeltaY) -> void
	{
		YawDegrees = std::remainder(YawDegrees + static_cast<double>(DeltaX) * RotationSensitivity, 360.0);
		PitchDegrees = std::clamp(PitchDegrees + static_cast<double>(DeltaY) * RotationSensitivity, -85.0, 85.0);
	}

	auto FOrbitAssetPreviewController::Pan(float DeltaX, float DeltaY) -> void
	{
		const double Yaw = Math::DegreesToRadians(YawDegrees);
		const FVector3 Right(-std::sin(Yaw), std::cos(Yaw), 0.0);
		const double Scale = Distance * PanSensitivity;
		Target += Right * (-static_cast<double>(DeltaX) * Scale);
		Target += FVectorConstants::Up * (static_cast<double>(DeltaY) * Scale);
	}

	auto FOrbitAssetPreviewController::Zoom(float WheelDelta) -> void
	{
		Distance = std::clamp(Distance * std::pow(ZoomScale, static_cast<double>(WheelDelta)),
			MinimumDistance, MaximumDistance);
	}

	auto FOrbitAssetPreviewController::Reset() -> void
	{
		if (FramedBounds.bIsValid) FrameBounds(FramedBounds);
	}

	auto FOrbitAssetPreviewController::ApplyInput(const FAssetPreviewViewportInput& Input) -> void
	{
		if (Input.bLeftDragging) Orbit(Input.MouseDeltaX, Input.MouseDeltaY);
		if (Input.bMiddleDragging) Pan(Input.MouseDeltaX, Input.MouseDeltaY);
		if (Input.MouseWheel != 0.0f) Zoom(Input.MouseWheel);
	}

	auto FOrbitAssetPreviewViewportClient::CalcSceneView(
		uint32 Width, uint32 Height, FSceneView& OutView) const -> bool
	{
		if (!IsPreviewEnabled() || Width == 0 || Height == 0) return false;
		const double Yaw = Math::DegreesToRadians(Controller.GetYawDegrees());
		const double Pitch = Math::DegreesToRadians(Controller.GetPitchDegrees());
		const double CosPitch = std::cos(Pitch);
		const FVector3 Eye = Controller.GetTarget() + FVector3(
			CosPitch * std::cos(Yaw), CosPitch * std::sin(Yaw), std::sin(Pitch))
			* Controller.GetDistance();
		const FVector3 Forward = Math::Normalize(Controller.GetTarget() - Eye);
		const FVector3 Right = Math::Normalize(Math::Cross(FVectorConstants::Up, Forward));
		const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));

		OutView = {};
		OutView.ViewportWidth = Width;
		OutView.ViewportHeight = Height;
		OutView.ViewLocation = Eye;
		OutView.ViewMatrix[0][0] = Forward.x;
		OutView.ViewMatrix[1][0] = Forward.y;
		OutView.ViewMatrix[2][0] = Forward.z;
		OutView.ViewMatrix[3][0] = -Math::Dot(Forward, Eye);
		OutView.ViewMatrix[0][1] = Right.x;
		OutView.ViewMatrix[1][1] = Right.y;
		OutView.ViewMatrix[2][1] = Right.z;
		OutView.ViewMatrix[3][1] = -Math::Dot(Right, Eye);
		OutView.ViewMatrix[0][2] = Up.x;
		OutView.ViewMatrix[1][2] = Up.y;
		OutView.ViewMatrix[2][2] = Up.z;
		OutView.ViewMatrix[3][2] = -Math::Dot(Up, Eye);

		const float NearClip = static_cast<float>(std::max(0.001, Controller.GetDistance() * 0.001));
		const float FarClip = static_cast<float>(std::max(100.0, Controller.GetDistance() * 20.0));
		if (!SceneViewProjection::BuildPerspectiveProjection(FieldOfViewDegrees,
			static_cast<double>(Width) / Height, NearClip, FarClip,
			ESceneDepthConvention::ReversedZ, OutView.ProjectionMatrix)) return false;
		OutView.NearClipDistance = NearClip;
		OutView.FarClipDistance = FarClip;
		OutView.DepthConvention = ESceneDepthConvention::ReversedZ;
		OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
		return true;
	}

	auto FOrbitAssetPreviewViewportClient::SetWireframe(bool bWireframe) -> void
	{
		FSceneViewSettings Settings = GetViewSettings();
		Settings.Mode.RasterMode = bWireframe ? ERasterMode::Wireframe : ERasterMode::Solid;
		SetViewSettings(Settings);
	}

	auto FOrbitAssetPreviewViewportClient::IsWireframe() const -> bool
	{
		return GetViewSettings().Mode.RasterMode == ERasterMode::Wireframe;
	}

	auto FOrbitAssetPreviewViewportClient::SetLit(bool bLit) -> void
	{
		FSceneViewSettings Settings = GetViewSettings();
		Settings.Mode.RenderMode = bLit ? ERenderMode::Lit : ERenderMode::Unlit;
		SetViewSettings(Settings);
	}

	auto FOrbitAssetPreviewViewportClient::IsLit() const -> bool
	{
		return GetViewSettings().Mode.RenderMode == ERenderMode::Lit;
	}
}
