#pragma once

#include "Client/ViewportClient.h"
#include "DObject/ObjectPtr.h"

namespace Durin
{
	class DCameraComponent;
}

namespace Durin::Editor::Level
{

	// Builds a preview scene view from one reflected camera component.
	class FCameraPreviewViewportClient final : public FViewportClient
	{
	public:
		auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override;
		auto SetCamera(DCameraComponent* InCamera) -> void { Camera = InCamera; }
		auto GetCamera() const -> DCameraComponent* { return Camera.Get(); }

	private:
		TObjectPtr<DCameraComponent> Camera;
	};
}
