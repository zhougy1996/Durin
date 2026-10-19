#pragma once

#include "CoreMinimal.h"

#include "LevelEditorViewportPicking.h"
#include "HitProxy.h"

namespace Durin
{
	class FSceneInterface;
	class DPrimitiveComponent;
}

namespace Durin::Editor::Level
{
	class FEditorVisualizationCollector;

	// Resolves a backend token through the request-local weak identity table.
	struct FViewportPickingTarget
	{
		uint32 Token = 0;
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DActorComponent> Component;
		uint64 RegistrationGeneration = 0;
		EViewportPickHitKind Kind = EViewportPickHitKind::SceneGeometry;
		FEditorSubElementSelection Element;
	};

	// Produces immutable CPU maps from numeric render inputs; identities and sampling stay in the service.
	class IViewportPickingBackend
	{
	public:
		virtual ~IViewportPickingBackend() = default;
		virtual auto GetHitProxyMap(FSceneInterface* Scene, FHitProxyRenderRequest Request) -> std::shared_ptr<const FHitProxyMap> = 0;
		virtual auto Invalidate() -> void {}
	};

	auto MakeHitProxyPickingBackend() -> std::unique_ptr<IViewportPickingBackend>;

	// Owns the viewport cache scope and resolves immediate tokens through weak identities.
	class FViewportPickingService final
	{
	public:
		FViewportPickingService();
		explicit FViewportPickingService(std::unique_ptr<IViewportPickingBackend> InBackend);
		auto SetLevel(DLevel* Level) -> void;
		auto Pick(const FViewportPickRequest& Request, const FEditorVisualizationCollector* Visualizations) -> FViewportPickResult;
		auto SetBackendForTesting(std::unique_ptr<IViewportPickingBackend> InBackend) -> void;
	private:
		auto Resolve(DLevel* Level, const std::vector<FViewportPickingTarget>& Targets,
			uint32 Token, double Distance) const -> FViewportPickResult;
		auto ValidateHit(DLevel* Level, const FViewportPickHit& Hit) const -> bool;
		std::unique_ptr<IViewportPickingBackend> Backend;
		TWeakObjectPtr<DLevel> CurrentLevel;
	};
} // namespace Durin::Editor::Level
