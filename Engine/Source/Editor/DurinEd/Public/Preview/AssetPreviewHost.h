#pragma once

#include "Client/ViewportClient.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class AActor;
	class DDirectionalLightComponent;
}

namespace Durin::Editor
{
	// Adds a common visibility gate to asset-specific preview viewport clients.
	class FAssetPreviewViewportClient : public FViewportClient
	{
	public:
		auto SetPreviewEnabled(bool bInEnabled) -> void { bPreviewEnabled = bInEnabled; }

	protected:
		auto IsPreviewEnabled() const -> bool { return bPreviewEnabled; }

	private:
		bool bPreviewEnabled = false;
	};

	// Describes one frame of pointer interaction with the rendered preview image.
	// Drag capture remains active after the pointer leaves the image until release.
	struct FAssetPreviewViewportInput
	{
		float MouseDeltaX = 0.0f;
		float MouseDeltaY = 0.0f;
		float MouseWheel = 0.0f;
		bool bHovered = false;
		bool bLeftDragging = false;
		bool bMiddleDragging = false;
	};

	struct FAssetPreviewHostConfig
	{
		FName SceneName;
		FName ContentActorName;
		FName LightActorName;
		FName LightComponentName = "PreviewLight";
		bool bBeginPlay = false;
	};

	// Owns the engine-neutral scene, lighting, offscreen viewport, and teardown
	// lifecycle shared by interactive asset previews.
	class FAssetPreviewHost final
	{
	public:
		DURINED_API FAssetPreviewHost(
			FAssetPreviewHostConfig Config,
			std::unique_ptr<FAssetPreviewViewportClient> ViewportClient);
		DURINED_API ~FAssetPreviewHost();

		FAssetPreviewHost(const FAssetPreviewHost&) = delete;
		FAssetPreviewHost& operator=(const FAssetPreviewHost&) = delete;

		DURINED_API auto IsAvailable() const -> bool;
		DURINED_API auto GetDiagnostic() const -> const std::string&;
		DURINED_API auto GetContentActor() const -> AActor*;
		DURINED_API auto SetVisible(bool bVisible) -> void;
		DURINED_API auto Tick(float DeltaSeconds) -> void;
		DURINED_API auto DrawViewport(float Width, float Height,
			FAssetPreviewViewportInput* OutInput = nullptr) -> bool;

	private:
		class FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
