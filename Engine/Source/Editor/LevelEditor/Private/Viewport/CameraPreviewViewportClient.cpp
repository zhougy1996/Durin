#include "Viewport/CameraPreviewViewportClient.h"

#include "Components/CameraComponent.h"
#include "IRendererModule.h"

namespace Durin
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
		return true;
	}
}
