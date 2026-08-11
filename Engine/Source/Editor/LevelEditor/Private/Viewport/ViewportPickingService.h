#pragma once

#include "LevelEditorViewportPicking.h"

namespace Durin
{
	class DPrimitiveComponent;

	// Resolves a backend token through the request-local weak identity table.
	struct FViewportPickingTarget
	{
		uint32 Token = 0;
		FPrimitiveSceneId PrimitiveId = InvalidPrimitiveSceneId;
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DPrimitiveComponent> Component;
		uint64 StableTieKey = 0;
		uint64 RegistrationGeneration = 0;
	};

	// Contains only owned values and weak identities safe to retain for deferred work.
	struct FViewportPickingBackendRequest
	{
		FViewportPickTicket Ticket;
		FVector3 RayOrigin{0.0};
		FVector3 RayDirection{0.0};
		std::vector<FViewportPickingTarget> Targets;
	};

	// Identifies a detached backend candidate by request-local token.
	struct FViewportPickingBackendHit
	{
		uint32 Token = 0;
		double Distance = std::numeric_limits<double>::max();
		int32 Priority = 0;
	};

	inline constexpr uint64 ViewportPickingMaximumSkinnedVertices = 250'000;
	inline constexpr uint64 ViewportPickingMaximumTestedTriangles = 500'000;

	// Bounds deterministic reference work without exposing it through the semantic picking API.
	struct FViewportPickingWorkBudget
	{
		uint64 MaximumSkinnedVertices = ViewportPickingMaximumSkinnedVertices;
		uint64 MaximumTestedTriangles = ViewportPickingMaximumTestedTriangles;
	};

	// Reports private reference-backend work for deterministic qualification and diagnosis.
	struct FViewportPickingBackendDiagnostics
	{
		uint64 ApplicableSkeletalTargets = 0;
		uint64 InvalidSkeletalTargets = 0;
		uint64 SkeletalBoundsRejects = 0;
		uint64 SkeletalBudgetFailures = 0;
		uint64 SkinnedVertices = 0;
		uint64 TestedTriangles = 0;
	};

	struct FViewportPickingBackendCompletion
	{
		EViewportPickStatus Status = EViewportPickStatus::Invalid;
		std::optional<FViewportPickingBackendHit> Hit;
		FViewportPickingBackendDiagnostics Diagnostics;
	};

	// Defines the complete-or-pending boundary used by CPU and deterministic fake backends.
	class IViewportPickingBackend
	{
	public:
		virtual ~IViewportPickingBackend() = default;
		virtual auto Submit(FViewportPickingBackendRequest Request) -> FViewportPickingBackendCompletion = 0;
		virtual auto Poll(FViewportPickTicket Ticket) -> FViewportPickingBackendCompletion = 0;
		virtual auto Cancel(FViewportPickTicket Ticket) -> void = 0;
	};

	// Creates the built-in immediate reference backend for focused private-contract tests.
	auto MakeReferenceViewportPickingBackend(FViewportPickingWorkBudget WorkBudget = {})
		-> std::unique_ptr<IViewportPickingBackend>;

	// Owns per-viewport ticket sequencing, weak target tables, validation, and arbitration.
	class FViewportPickingService final
	{
	public:
		FViewportPickingService();
		explicit FViewportPickingService(std::unique_ptr<IViewportPickingBackend> InBackend);
		~FViewportPickingService();

		auto SetLevel(DLevel* Level) -> void;
		auto Submit(FViewportPickRequest Request, std::optional<FViewportPickHit> Visualization) -> FViewportPickSubmission;
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
			std::optional<FViewportPickHit> Visualization;
			uint64 VisualizationRegistrationGeneration = 0;
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
} // namespace Durin
