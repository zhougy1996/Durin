#pragma once

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
		uint64 StableTieKey = 0;
		uint64 RegistrationGeneration = 0;
		EViewportPickHitKind Kind = EViewportPickHitKind::SceneGeometry;
		FEditorSubElementSelection Element;
		bool bForeground = false;
		int32 Priority = 0;
	};

	// Contains only owned values and weak identities safe to retain for deferred work.
	struct FViewportPickingBackendRequest
	{
		FViewportPickTicket Ticket;
		FVector3 RayOrigin{0.0};
		FVector3 RayDirection{0.0};
		std::vector<FViewportPickingTarget> Targets;
		FSceneView View;
		FVector2f Position{0.f};
		FSceneInterface* Scene = nullptr;
		std::vector<FHitProxyOverlay> Overlays;
		bool bSceneGeometry = true;
	};

	// Identifies a detached backend candidate by request-local token.
	struct FViewportPickingBackendHit
	{
		uint32 Token = 0;
		double Distance = std::numeric_limits<double>::max();
		int32 Priority = 0;
	};

	struct FViewportPickingBackendCompletion
	{
		EViewportPickStatus Status = EViewportPickStatus::Invalid;
		std::optional<FViewportPickingBackendHit> Hit;
	};

	// Defines the complete-or-pending boundary used by GPU readback and deterministic test backends.
	class IViewportPickingBackend
	{
	public:
		virtual ~IViewportPickingBackend() = default;
		virtual auto Submit(FViewportPickingBackendRequest Request) -> FViewportPickingBackendCompletion = 0;
		virtual auto Poll(FViewportPickTicket Ticket) -> FViewportPickingBackendCompletion = 0;
		virtual auto Cancel(FViewportPickTicket Ticket) -> void = 0;
	};

	auto MakeHitProxyPickingBackend() -> std::unique_ptr<IViewportPickingBackend>;

	// Owns per-viewport ticket sequencing, weak target tables, validation, and arbitration.
	class FViewportPickingService final
	{
	public:
		FViewportPickingService();
		explicit FViewportPickingService(std::unique_ptr<IViewportPickingBackend> InBackend);
		~FViewportPickingService();

		auto SetLevel(DLevel* Level) -> void;
		auto Submit(FViewportPickRequest Request, const FEditorVisualizationCollector* Visualizations) -> FViewportPickSubmission;
		auto Poll(FViewportPickTicket Ticket) -> FViewportPickCompletion;
		auto Cancel(FViewportPickTicket Ticket) -> void;
		auto Release(FViewportPickTicket Ticket) -> void;
		auto Invalidate() -> void;
		auto GetGeneration() const -> uint64 { return Generation; }
		auto SetBackendForTesting(std::unique_ptr<IViewportPickingBackend> InBackend) -> void;

	private:
		struct FRequestRecord
		{
			TWeakObjectPtr<DLevel> Level;
			uint64 Generation = 0;
			std::vector<FViewportPickingTarget> Targets;
			std::optional<FViewportPickCompletion> Completion;
		};

		auto Complete(FRequestRecord& Record, const FViewportPickingBackendCompletion& BackendCompletion) const -> FViewportPickCompletion;
		auto ValidateHit(const FRequestRecord& Record, const FViewportPickHit& Hit) const -> bool;
		auto MakeTerminal(EViewportPickStatus Status) const -> FViewportPickCompletion;

		std::unique_ptr<IViewportPickingBackend> Backend;
		TWeakObjectPtr<DLevel> CurrentLevel;
		uint64 Generation = 1;
		uint64 NextTicketId = 1;
		std::unordered_map<uint64, FRequestRecord> Requests;
		std::unordered_map<EViewportPickPurpose, FViewportPickTicket> PurposeTickets;
	};
} // namespace Durin::Editor::Level
