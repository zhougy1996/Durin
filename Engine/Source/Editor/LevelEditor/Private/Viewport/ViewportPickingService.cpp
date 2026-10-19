#include "Viewport/ViewportPickingService.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"
#include "RenderingThread.h"
#include "SceneViewProjection.h"
#include "LevelEditorCustomizations.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto BuildHitProxyOverlays(const FEditorVisualizationCollector& Collector, const FSceneView& View,
			std::vector<FViewportPickingTarget>& Targets, std::vector<FHitProxyOverlay>& Overlays) -> bool
		{
			bool bOverflow = false;
			const auto AddQuad = [&](const auto& Source, std::array<FVector4, 4> Corners,
				std::array<float, 4> Distances, bool bForeground) {
				auto* Actor = Source.Actor.Get();
				auto* Component = Source.Component.Get();
				if (!Actor || !Component || !Component->IsRegistered() || Actor->IsHidden()) return;
				if (Overlays.size() >= 65536) { bOverflow = true; return; }
				const uint32 Token = uint32(Targets.size() + 1);
				Targets.push_back({Token, FPrimitiveComponentId{}, Actor, Component,
					Component->GetRegistrationGeneration(), EViewportPickHitKind::EditorVisualization,
					Source.Element});
				FHitProxyOverlay Overlay;
				Overlay.Id = FHitProxyId{Token};
				Overlay.bForeground = bForeground;
				Overlay.Priority = Source.HitPriority;
				constexpr uint32 Indices[]{0, 1, 2, 0, 2, 3};
				for (uint32 I = 0; I < 6; ++I)
					Overlay.Vertices[I] = {FVector4f(Corners[Indices[I]]), Distances[Indices[I]]};
				Overlays.push_back(std::move(Overlay));
			};
			const auto AddBox = [&](const auto& Source) {
				FVector4 Clip = View.ViewProjectionMatrix * FVector4(Source.WorldPosition, 1.0);
				if (!Math::IsFinite(Clip) || Clip.w <= 0.0 || Clip.z < 0.0 || Clip.z > Clip.w) return;
				Clip /= Clip.w;
				const double Half = Source.SizePixels * 0.5 + Source.HitPaddingPixels;
				if (!std::isfinite(Half) || Half <= 0.0) return;
				const FVector4 X(2.0 * Half / View.ViewportWidth, 0.0, 0.0, 0.0);
				const FVector4 Y(0.0, 2.0 * Half / View.ViewportHeight, 0.0, 0.0);
				const float Distance = float(Math::Length(Source.WorldPosition - View.ViewLocation));
				AddQuad(Source, {Clip-X-Y, Clip+X-Y, Clip+X+Y, Clip-X+Y},
					{Distance, Distance, Distance, Distance}, Source.bDepthIndependentHit);
			};
			for (const auto& Icon : Collector.GetIcons()) AddBox(Icon);
			for (const auto& Box : Collector.GetBoxes()) AddBox(Box);
			for (const auto& Line : Collector.GetLines())
			{
				FVector4 A = View.ViewProjectionMatrix * FVector4(Line.Start, 1.0);
				FVector4 B = View.ViewProjectionMatrix * FVector4(Line.End, 1.0);
				if (!Math::IsFinite(A) || !Math::IsFinite(B)) continue;
				float DA = float(Math::Length(Line.Start - View.ViewLocation));
				float DB = float(Math::Length(Line.End - View.ViewLocation));
				bool bVisible = true;
				for (uint32 Plane = 0; Plane < 3; ++Plane)
				{
					const auto Evaluate = [Plane](const FVector4& P) { return Plane == 0 ? P.w - 1.e-8 : Plane == 1 ? P.z : P.w-P.z; };
					const double FA = Evaluate(A), FB = Evaluate(B);
					if (FA < 0.0 && FB < 0.0) { bVisible = false; break; }
					if ((FA < 0.0) != (FB < 0.0))
					{
						const double T = FA / (FA - FB);
						const FVector4 C = A + (B-A)*T;
						const float DC = DA + (DB-DA)*float(T);
						if (FA < 0.0) { A=C; DA=DC; } else { B=C; DB=DC; }
					}
				}
				if (!bVisible) continue;
				A /= A.w; B /= B.w;
				const FVector2 Delta((B.x-A.x)*View.ViewportWidth, (B.y-A.y)*View.ViewportHeight);
				const double Length = Math::Length(Delta);
				if (Length <= 1.e-8 || !std::isfinite(Line.HitTolerancePixels)) continue;
				const double Radius = std::max(double(Line.WidthPixels)*0.5, double(Line.HitTolerancePixels));
				const FVector4 Offset(-Delta.y/Length*2.0*Radius/View.ViewportWidth, Delta.x/Length*2.0*Radius/View.ViewportHeight, 0.0, 0.0);
				AddQuad(Line, {A-Offset, B-Offset, B+Offset, A+Offset}, {DA, DB, DB, DA}, false);
			}
			return !bOverflow;
		}

		class FHitProxyPickingBackend final : public IViewportPickingBackend
		{
		public:
			auto GetHitProxyMap(FSceneInterface* Scene, FHitProxyRenderRequest Request) -> std::shared_ptr<const FHitProxyMap> override
			{
				auto* Renderer = GEngine ? GEngine->GetRendererModule() : nullptr;
				if (!Renderer || (Request.bSceneGeometry && !Scene)) return {};
				Request.Cache = Cache;
				const bool bInvalidate = std::exchange(bInvalidateCache, false);
				std::shared_ptr<const FHitProxyMap> Map;
				ENQUEUE_RENDER_COMMAND(ReadViewportHitProxies)([Renderer, Scene, Request = std::move(Request), bInvalidate, &Map](FRHICommandListImmediate& Commands) {
					if (bInvalidate) Request.Cache->Invalidate_RenderThread();
					Map = Renderer->RenderHitProxies(Commands, Scene, Request);
				});
				FlushRenderingCommands();
				return Map;
			}
			auto Invalidate() -> void override { bInvalidateCache = true; }
		private:
			std::shared_ptr<FHitProxyMapCache> Cache = std::make_shared<FHitProxyMapCache>();
			bool bInvalidateCache = false;
		};
	}

	auto MakeHitProxyPickingBackend() -> std::unique_ptr<IViewportPickingBackend>
	{ return std::make_unique<FHitProxyPickingBackend>(); }

	FViewportPickingService::FViewportPickingService()
		: Backend(MakeHitProxyPickingBackend()) {}

	FViewportPickingService::FViewportPickingService(std::unique_ptr<IViewportPickingBackend> InBackend)
		: Backend(InBackend ? std::move(InBackend) : MakeHitProxyPickingBackend()) {}

	auto FViewportPickingService::SetLevel(DLevel* Level) -> void
	{
		Backend->Invalidate();
		CurrentLevel = Level;
	}

	auto FViewportPickingService::Pick(const FViewportPickRequest& Request, const FEditorVisualizationCollector* Visualizations) -> FViewportPickResult
	{
		DLevel* Level = Request.Level.Get();
		if (!Level || Level != CurrentLevel.Get()) return {EViewportPickStatus::Invalid};
		FVector3 RayOrigin, RayDirection;
		if (!SceneViewProjection::BuildViewportRay(Request.View, Request.ViewportPosition, RayOrigin, RayDirection)
			|| !Math::IsFinite(RayOrigin) || !Math::IsFinite(RayDirection)) return {EViewportPickStatus::Invalid};

		FHitProxyRenderRequest Render;
		Render.View = Request.View;
		Render.bSceneGeometry = EnumHasAnyFlags(Request.Layers, EViewportPickLayer::SceneGeometry);
		std::vector<FViewportPickingTarget> Targets;
		if (EnumHasAnyFlags(Request.Layers, EViewportPickLayer::SceneGeometry))
		{
			for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
			{
				AActor* Actor = ActorPtr.Get();
				if (!Actor || Actor->IsHidden()) continue;
				for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetComponents())
				{
					auto* Component = Cast<DPrimitiveComponent>(ComponentPtr.Get());
					if (!Component || !Component->IsRegistered() || !Component->IsVisible()) continue;
					const FPrimitiveComponentId PrimitiveId = Component->GetPrimitiveComponentId();
					if (PrimitiveId == InvalidPrimitiveComponentId) continue;
					Targets.push_back({static_cast<uint32>(Targets.size() + 1), PrimitiveId,
						Actor, Component, Component->GetRegistrationGeneration()});
				}
			}
		}
		std::vector<FHitProxyOverlay> Overlays;
		if (Visualizations && EnumHasAnyFlags(Request.Layers, EViewportPickLayer::EditorVisualization)
			&& !BuildHitProxyOverlays(*Visualizations, Request.View, Targets, Overlays)) return {EViewportPickStatus::Failed};
		if (!EnumHasAnyFlags(Request.Layers, EViewportPickLayer::SceneGeometry) && Overlays.empty()) return {EViewportPickStatus::Completed};
		for (const auto& Target : Targets)
			if (Target.Kind == EViewportPickHitKind::SceneGeometry) Render.Primitives.push_back({Target.PrimitiveId.Value, FHitProxyId{Target.Token}});
		Render.Overlays = std::move(Overlays);
		const auto Map = Backend->GetHitProxyMap(Level->GetWorld() ? Level->GetWorld()->GetRenderScene() : nullptr, std::move(Render));
		FHitProxyPixel Pixel;
		if (!Map || !Map->ReadPixel(uint32(Request.ViewportPosition.x), uint32(Request.ViewportPosition.y), Pixel)) return {EViewportPickStatus::Failed};
		if (!Pixel.Id) return {EViewportPickStatus::Completed};
		return Resolve(Level, Targets, Pixel.Id.Value, double(Pixel.Distance));
	}

	auto FViewportPickingService::SetBackendForTesting(std::unique_ptr<IViewportPickingBackend> InBackend) -> void
	{ Backend = InBackend ? std::move(InBackend) : MakeHitProxyPickingBackend(); }

	auto FViewportPickingService::Resolve(DLevel* Level, const std::vector<FViewportPickingTarget>& Targets,
		uint32 Token, double Distance) const -> FViewportPickResult
	{
		const auto It = std::ranges::find(Targets, Token, &FViewportPickingTarget::Token);
		if (It == Targets.end()) return {EViewportPickStatus::Failed};
		FViewportPickHit Hit{
			.Kind = It->Kind,
			.PrimitiveId = It->PrimitiveId,
			.Actor = It->Actor,
			.Component = It->Component.Get(),
			.Element = It->Element,
			.Distance = Distance,
		};
		if (!std::isfinite(Hit.Distance) || Hit.Distance < 0.0) return {EViewportPickStatus::Failed};
		if (!It->Component.Get() || It->Component.Get()->GetRegistrationGeneration() != It->RegistrationGeneration
			|| !ValidateHit(Level, Hit)) return {EViewportPickStatus::Invalid};
		return {EViewportPickStatus::Completed, Hit};
	}

	auto FViewportPickingService::ValidateHit(DLevel* Level, const FViewportPickHit& Hit) const -> bool
	{
		AActor* Actor = Hit.Actor.Get();
		DActorComponent* Component = Hit.Component.Get();
		if (!Level || Level != CurrentLevel.Get() || !Actor || !Component
			|| Actor->IsHidden() || !Level->ContainsActor(Actor) || !Actor->OwnsComponent(Component)
			|| Component->GetOwner() != Actor || !Component->IsRegistered() || Component->IsBeingDestroyed()) return false;
		if (Hit.Kind == EViewportPickHitKind::SceneGeometry)
		{
			auto* Primitive = Cast<DPrimitiveComponent>(Component);
			if (!Primitive || !Primitive->IsVisible() || Hit.PrimitiveId == InvalidPrimitiveComponentId || Primitive->GetPrimitiveComponentId() != Hit.PrimitiveId) return false;
		}
		return true;
	}
} // namespace Durin::Editor::Level
