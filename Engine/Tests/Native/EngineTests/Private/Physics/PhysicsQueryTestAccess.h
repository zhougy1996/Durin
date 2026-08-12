#pragma once

#include "Physics/PhysicsScene.h"

namespace Durin
{
	struct FPhysicsSceneQueryTestAccess
	{
		static auto ClearFault(FPhysicsScene& Scene) -> void
		{
			Scene.ProductionTestFault = FPhysicsScene::EProductionTestFault::None;
		}

		static auto OmitFirstCandidate(FPhysicsScene& Scene) -> void
		{
			Scene.ProductionTestFault = FPhysicsScene::EProductionTestFault::OmitFirstCandidate;
		}

		static auto ReverseCandidates(FPhysicsScene& Scene) -> void
		{
			Scene.ProductionTestFault = FPhysicsScene::EProductionTestFault::ReverseCandidates;
		}

		static auto ReverseResults(FPhysicsScene& Scene) -> void
		{
			Scene.ProductionTestFault = FPhysicsScene::EProductionTestFault::ReverseResults;
		}

		static auto CorruptFirstResult(FPhysicsScene& Scene) -> void
		{
			Scene.ProductionTestFault = FPhysicsScene::EProductionTestFault::CorruptFirstResult;
		}

		static auto ForceScratchOverflow(FPhysicsScene& Scene) -> void
		{
			Scene.ProductionTestFault = FPhysicsScene::EProductionTestFault::ForceScratchOverflow;
		}

		static auto GetMismatchCount(const FPhysicsScene& Scene) -> uint64
		{
			uint64 Result = 0;
			for (const FPhysicsSceneQueryCounters& Counters : Scene.Diagnostics.Queries)
				Result += Counters.CompareMismatches;
			return Result;
		}

		static auto GetLastDifferenceMask(const FPhysicsScene& Scene) -> uint32
		{
			return Scene.Diagnostics.LastMismatch.DifferenceMask;
		}

		static auto SaturateSubmittedQueries(FPhysicsScene& Scene) -> void
		{
			Scene.Diagnostics.Queries[static_cast<size_t>(EPhysicsSceneQueryKind::LineTraceSingle)]
				.SubmittedQueries = std::numeric_limits<uint64>::max();
		}

		static auto GetSceneSize() -> size_t { return sizeof(FPhysicsScene); }
		static constexpr auto GetBodyRecordSize() -> size_t { return sizeof(FPhysicsScene::FBodyRecord); }
		static auto GetDiagnosticsSize() -> size_t { return sizeof(FPhysicsSceneQueryDiagnostics); }
		static auto GetMismatchSize() -> size_t { return sizeof(FPhysicsSceneQueryMismatch); }
		static constexpr auto GetSlotSize() -> size_t { return sizeof(FPhysicsScene::FSlot); }
		static constexpr auto GetSpatialNodeSize() -> size_t { return sizeof(FPhysicsScene::FSpatialNode); }

		static auto GetBodyBounds(const FPhysicsScene& Scene, FPhysicsActorHandle Handle)
			-> std::array<float, 6>
		{
			if (!Handle.IsValid() || Handle.Id > Scene.Slots.size()) return {};
			const FPhysicsScene::FSlot& Slot = Scene.Slots[static_cast<size_t>(Handle.Id - 1)];
			if (Slot.Generation != Handle.Generation || Slot.DenseOrNext >= Scene.Bodies.size()) return {};
			return Scene.Bodies[Slot.DenseOrNext].Bounds;
		}
	};
}
