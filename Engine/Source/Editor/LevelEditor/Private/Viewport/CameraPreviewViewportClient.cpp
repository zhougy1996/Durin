#include "Viewport/CameraPreviewViewportClient.h"

#include "Components/CameraComponent.h"
#include "SceneView.h"
#include "SceneViewProjection.h"

namespace Durin::Editor::Level
{
	auto FCameraPreviewViewportClient::CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool
	{
		const DCameraComponent* CameraComponent = Camera.Get();
		if (CameraComponent == nullptr || Width == 0 || Height == 0) return false;

		const float AspectRatio = CameraComponent->ResolveAspectRatio(static_cast<float>(Width) / static_cast<float>(Height));
		OutView = {};
		OutView.ViewportWidth = Width;
		OutView.ViewportHeight = Height;
		if (CameraComponent->GetAspectRatioMode() != ECameraAspectRatioMode::Viewport) OutView.AspectRatioConstraint = AspectRatio;
		OutView.ViewMatrix = CameraComponent->GetViewMatrix();
		OutView.ProjectionMatrix = CameraComponent->GetProjectionMatrix(AspectRatio);
		OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
		OutView.ViewLocation = CameraComponent->GetWorldLocation();
		OutView.DepthConvention = ESceneDepthConvention::ReversedZ;
		const FCameraProjectionSettings& Projection =
			CameraComponent->GetProjectionSettings();
		OutView.NearClipDistance = Projection.NearClip;
		OutView.FarClipDistance = Projection.FarClip;
		const FViewDistanceSettings& ViewDistance =
			CameraComponent->GetViewDistance();
		SceneViewProjection::ClampViewDistances(OutView.FarClipDistance,
			ViewDistance.FadeStart, ViewDistance.RenderDistance,
			OutView.ViewFadeStart, OutView.ViewRenderDistance);
		return true;
	}
}
