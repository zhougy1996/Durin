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
#include <cstring>

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
				Targets.push_back({Token, FPrimitiveComponentId{}, Actor, Component, TWeakObjectPtr<DActorComponent>(Component).GetKey().GetHash(),
					Component->GetRegistrationGeneration(), EViewportPickHitKind::EditorVisualization,
					Source.Element, bForeground, Source.HitPriority});
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
			auto Submit(FViewportPickingBackendRequest Request) -> FViewportPickingBackendCompletion override
			{
				auto* Renderer = GEngine ? GEngine->GetRendererModule() : nullptr;
				if (!Renderer || (Request.bSceneGeometry && !Request.Scene)) return {EViewportPickStatus::Failed};
				FHitProxyRenderRequest Render;
				Render.View = Request.View;
				Render.Overlays = std::move(Request.Overlays);
				Render.bSceneGeometry = Request.bSceneGeometry;
				Render.Readback = std::make_shared<FRHITextureReadback>();
				for (const auto& Target : Request.Targets)
					if (Target.Kind == EViewportPickHitKind::SceneGeometry) Render.Primitives.push_back({Target.PrimitiveId.Value, FHitProxyId{Target.Token}});
				Pending.emplace(Request.Ticket.Id, FPending{Render.Readback,
					Request.View.ViewportX + Request.View.ViewportWidth,
					Request.View.ViewportY + Request.View.ViewportHeight,
					uint32(Request.Position.x), uint32(Request.Position.y)});
				ENQUEUE_RENDER_COMMAND(RenderViewportHitProxies)([Renderer, Scene = Request.Scene, Render = std::move(Render)](FRHICommandListImmediate& Commands) {
					Renderer->RenderHitProxies(Commands, Scene, Render);
				});
				return {EViewportPickStatus::Pending};
			}
			auto Poll(FViewportPickTicket Ticket) -> FViewportPickingBackendCompletion override
			{
				const auto It = Pending.find(Ticket.Id);
				if (It == Pending.end()) return {EViewportPickStatus::Invalid};
				const auto& Entry = It->second;
				const auto State = Entry.Readback->GetState();
				if (State == ERHITextureReadbackState::Pending) return {EViewportPickStatus::Pending};
				FViewportPickingBackendCompletion Result{EViewportPickStatus::Failed};
				FByteBuffer Pixels;
				if (Entry.Readback->TakePixels(Pixels) && Entry.X < Entry.Width && Entry.Y < Entry.Height
					&& Pixels.size() == uint64(Entry.Width) * Entry.Height * 8)
				{
					std::array<uint32, 2> Pixel;
					std::memcpy(Pixel.data(), Pixels.data() + (uint64(Entry.Y) * Entry.Width + Entry.X) * 8, sizeof(Pixel));
					Result.Status = EViewportPickStatus::Completed;
					if (Pixel[0]) Result.Hit = FViewportPickingBackendHit{Pixel[0], double(std::bit_cast<float>(Pixel[1])), 0};
				}
				Pending.erase(It);
				return Result;
			}
			auto Cancel(FViewportPickTicket Ticket) -> void override
			{
				if (const auto It = Pending.find(Ticket.Id); It != Pending.end())
				{ It->second.Readback->Cancel(); Pending.erase(It); }
			}
		private:
			struct FPending
			{
				std::shared_ptr<FRHITextureReadback> Readback;
				uint32 Width, Height, X, Y;
			};
			std::unordered_map<uint64, FPending> Pending;
		};
	}

	auto MakeHitProxyPickingBackend() -> std::unique_ptr<IViewportPickingBackend>
	{
		return std::make_unique<FHitProxyPickingBackend>();
	}

	FViewportPickingService::FViewportPickingService()
		: Backend(MakeHitProxyPickingBackend())
	{
	}

	FViewportPickingService::FViewportPickingService(std::unique_ptr<IViewportPickingBackend> InBackend)
		: Backend(InBackend ? std::move(InBackend)
			: MakeHitProxyPickingBackend())
	{
	}

	FViewportPickingService::~FViewportPickingService()
	{
		Invalidate();
	}

	auto FViewportPickingService::SetLevel(DLevel* Level) -> void
	{
		Invalidate();
		CurrentLevel = Level;
	}

	auto FViewportPickingService::Submit(FViewportPickRequest Request, const FEditorVisualizationCollector* Visualizations) -> FViewportPickSubmission
	{
		const FViewportPickTicket Ticket{NextTicketId++};
		if (const auto It = PurposeTickets.find(Request.Purpose); It != PurposeTickets.end()) Cancel(It->second);
		PurposeTickets[Request.Purpose] = Ticket;
		if (!Ticket || !Request.Level.Get() || Request.Level.Get() != CurrentLevel.Get()
			|| Request.Precision != EViewportPickPrecision::ActorComponentSurface)
			return {Ticket, MakeTerminal(EViewportPickStatus::Invalid)};
		FVector3 RayOrigin;
		FVector3 RayDirection;
		if (!SceneViewProjection::BuildViewportRay(Request.View, Request.ViewportPosition, RayOrigin, RayDirection)
			|| !Math::IsFinite(RayOrigin) || !Math::IsFinite(RayDirection))
			return {Ticket, MakeTerminal(EViewportPickStatus::Invalid)};

		Request.RequestId = Ticket.Id;
		Request.ViewportGeneration = Generation;

		FRequestRecord Record;
		Record.Level = Request.Level;
		Record.Generation = Generation;

		if (EnumHasAnyFlags(Request.Layers, EViewportPickLayer::SceneGeometry))
		{
			DLevel* Level = Request.Level.Get();
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
					Record.Targets.push_back({static_cast<uint32>(Record.Targets.size() + 1), PrimitiveId,
						Actor, Component, PrimitiveId.Value, Component->GetRegistrationGeneration()});
				}
			}
		}

		std::vector<FHitProxyOverlay> Overlays;
		if (Visualizations && EnumHasAnyFlags(Request.Layers, EViewportPickLayer::EditorVisualization)
			&& !BuildHitProxyOverlays(*Visualizations, Request.View, Record.Targets, Overlays))
			return {Ticket, MakeTerminal(EViewportPickStatus::Failed)};
		FViewportPickingBackendCompletion BackendCompletion{EViewportPickStatus::Completed, std::nullopt};
		if (EnumHasAnyFlags(Request.Layers, EViewportPickLayer::SceneGeometry) || !Overlays.empty())
			BackendCompletion = Backend->Submit({Ticket, RayOrigin, RayDirection, Record.Targets, Request.View, Request.ViewportPosition,
				Request.Level.Get()->GetWorld() ? Request.Level.Get()->GetWorld()->GetRenderScene() : nullptr,
				std::move(Overlays), EnumHasAnyFlags(Request.Layers, EViewportPickLayer::SceneGeometry)});
		FViewportPickCompletion Completion = Complete(Record, BackendCompletion);
		if (Completion.Status != EViewportPickStatus::Pending) Record.Completion = Completion;
		Requests.emplace(Ticket.Id, std::move(Record));
		return {Ticket, Completion};
	}

	auto FViewportPickingService::Poll(FViewportPickTicket Ticket) -> FViewportPickCompletion
	{
		const auto It = Requests.find(Ticket.Id);
		if (!Ticket || It == Requests.end()) return MakeTerminal(EViewportPickStatus::Invalid);
		FRequestRecord& Record = It->second;
		if (Record.Completion) return *Record.Completion;
		if (Record.Generation != Generation || Record.Level.Get() != CurrentLevel.Get())
		{
			Record.Completion = MakeTerminal(EViewportPickStatus::Invalidated);
			return *Record.Completion;
		}
		const FViewportPickingBackendCompletion BackendCompletion = Backend->Poll(Ticket);
		FViewportPickCompletion Completion = Complete(Record, BackendCompletion);
		if (Completion.Status != EViewportPickStatus::Pending) Record.Completion = Completion;
		return Completion;
	}

	auto FViewportPickingService::Cancel(FViewportPickTicket Ticket) -> void
	{
		const auto It = Requests.find(Ticket.Id);
		if (!Ticket || It == Requests.end()) return;
		Backend->Cancel(Ticket);
		It->second.Completion = MakeTerminal(EViewportPickStatus::Cancelled);
	}

	auto FViewportPickingService::Release(FViewportPickTicket Ticket) -> void
	{
		if (!Ticket) return;
		for (auto It = PurposeTickets.begin(); It != PurposeTickets.end();)
			if (It->second == Ticket) It = PurposeTickets.erase(It); else ++It;
		Backend->Cancel(Ticket);
		Requests.erase(Ticket.Id);
	}

	auto FViewportPickingService::Invalidate() -> void
	{
		for (auto& [Id, Record] : Requests)
		{
			Backend->Cancel({Id});
			Record.Completion = MakeTerminal(EViewportPickStatus::Invalidated);
		}
		PurposeTickets.clear();
		++Generation;
	}

	auto FViewportPickingService::SetBackendForTesting(std::unique_ptr<IViewportPickingBackend> InBackend) -> void
	{
		Invalidate();
		Backend = InBackend ? std::move(InBackend)
			: MakeHitProxyPickingBackend();
	}

	auto FViewportPickingService::Complete(FRequestRecord& Record,
		const FViewportPickingBackendCompletion& BackendCompletion) const -> FViewportPickCompletion
	{
		if (BackendCompletion.Status == EViewportPickStatus::Pending) return MakeTerminal(EViewportPickStatus::Pending);
		if (BackendCompletion.Status != EViewportPickStatus::Completed) return MakeTerminal(BackendCompletion.Status);
		std::optional<FViewportPickHit> Winner;
		if (BackendCompletion.Hit)
		{
			const auto It = std::ranges::find(Record.Targets, BackendCompletion.Hit->Token, &FViewportPickingTarget::Token);
			if (It == Record.Targets.end()) return MakeTerminal(EViewportPickStatus::Failed);
			FViewportPickHit Geometry{
				.Kind = It->Kind,
				.PrimitiveId = It->PrimitiveId,
				.Actor = It->Actor,
				.Component = It->Component.Get(),
				.Element = It->Element,
				.Distance = BackendCompletion.Hit->Distance,
				.Priority = It->Priority,
				.StableTieKey = It->StableTieKey,
				.bDepthIndependent = It->bForeground,
			};
			if (!It->Component.Get() || It->Component.Get()->GetRegistrationGeneration() != It->RegistrationGeneration) return MakeTerminal(EViewportPickStatus::Invalidated);
			if (!std::isfinite(Geometry.Distance) || Geometry.Distance < 0.0) return MakeTerminal(EViewportPickStatus::Failed);
			Winner = Geometry;
		}
		if (Winner && !ValidateHit(Record, *Winner)) return MakeTerminal(EViewportPickStatus::Invalidated);
		return {EViewportPickStatus::Completed, Winner};
	}

	auto FViewportPickingService::ValidateHit(const FRequestRecord& Record, const FViewportPickHit& Hit) const -> bool
	{
		DLevel* Level = Record.Level.Get();
		AActor* Actor = Hit.Actor.Get();
		DActorComponent* Component = Hit.Component.Get();
		if (!Level || Level != CurrentLevel.Get() || Record.Generation != Generation || !Actor || !Component
			|| Actor->IsHidden() || !Level->ContainsActor(Actor) || !Actor->OwnsComponent(Component)
			|| Component->GetOwner() != Actor || !Component->IsRegistered() || Component->IsBeingDestroyed()) return false;
		if (Hit.Kind == EViewportPickHitKind::SceneGeometry)
		{
			auto* Primitive = Cast<DPrimitiveComponent>(Component);
			if (!Primitive || !Primitive->IsVisible() || Hit.PrimitiveId == InvalidPrimitiveComponentId || Primitive->GetPrimitiveComponentId() != Hit.PrimitiveId) return false;
			const auto Target = std::ranges::find(Record.Targets, Hit.PrimitiveId, &FViewportPickingTarget::PrimitiveId);
			if (Target == Record.Targets.end() || Primitive->GetRegistrationGeneration() != Target->RegistrationGeneration) return false;
		}
		return true;
	}

	auto FViewportPickingService::MakeTerminal(EViewportPickStatus Status) const -> FViewportPickCompletion
	{
		return {Status, std::nullopt};
	}
} // namespace Durin::Editor::Level
