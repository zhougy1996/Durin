#pragma once

#include "DObject/ObjectPtr.h"
#include "Engine/PrimitiveSceneChanges.h"
#include "Math/Box.h"

namespace Durin
{
	class DLevel;
	class AActor;
	class DPrimitiveComponent;
}

namespace Durin::Editor::Level
{

	// Captured by the editor at notification time, before later scene mutations.
	struct FViewportPickingMutation : FPrimitiveSceneMutation
	{
		FBox WorldBounds;
		bool bVisible = false;
	};

	struct FViewportPickingMutationBatch
	{
		uint64 Revision = 0;
		bool bCompleteSnapshot = false;
		std::vector<FViewportPickingMutation> Mutations;
	};

	struct FViewportPickingSceneCandidate
	{
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DPrimitiveComponent> Component;
		uint64 StableTieKey = 0;
		uint64 RegistrationGeneration = 0;
	};

	struct FViewportPickingSceneIndexDiagnostics
	{
		uint64 SnapshotBuilds = 0;
		uint64 Mutations = 0;
		uint64 Reinsertions = 0;
		uint64 Rebuilds = 0;
		uint64 BuildNanoseconds = 0;
		uint64 RetainedBytes = 0;
		uint64 BoundsTests = 0;
		uint64 NodeVisits = 0;
		uint64 CandidatePrimitives = 0;
		uint64 ReferenceFallbacks = 0;
	};

	// Maintains one deterministic game-thread broad phase for all viewports of a level context.
	class FViewportPickingSceneIndex final
		: public std::enable_shared_from_this<FViewportPickingSceneIndex>
	{
	public:
		FViewportPickingSceneIndex();
		~FViewportPickingSceneIndex();
		FViewportPickingSceneIndex(const FViewportPickingSceneIndex&) = delete;
		auto operator=(const FViewportPickingSceneIndex&) -> FViewportPickingSceneIndex& = delete;

		auto SetLevel(DLevel* Level) -> void;
		// Returns false when the complete sequence or memory contract cannot be proven.
		auto QueryRay(const FVector3& Origin, const FVector3& Direction,
			std::vector<FViewportPickingSceneCandidate>& OutCandidates) -> bool;
		auto GetDiagnostics() const -> const FViewportPickingSceneIndexDiagnostics& { return Diagnostics; }
		auto GetLevel() const -> DLevel*;

	private:
		struct FLeaf
		{
			FViewportPickingSceneCandidate Candidate;
			FBox ExactBounds;
			FBox FatBounds;
			uint32 NodeIndex = std::numeric_limits<uint32>::max();
		};

		struct FNode
		{
			FBox Bounds;
			uint32 Left = std::numeric_limits<uint32>::max();
			uint32 Right = std::numeric_limits<uint32>::max();
			uint32 Parent = std::numeric_limits<uint32>::max();
			FPrimitiveComponentId LeafId = InvalidPrimitiveComponentId;
		};

		auto Retire() -> void;
		auto ReceiveBatch(const FPrimitiveSceneMutationBatch& Batch) -> void;
		auto Synchronize() -> bool;
		auto ApplySnapshot(const FViewportPickingMutationBatch& Batch) -> bool;
		auto ApplyMutation(const FViewportPickingMutation& Mutation) -> bool;
		auto Rebuild() -> bool;
		auto BuildRange(std::vector<FPrimitiveComponentId>& Ids, size_t Begin, size_t End, uint32 Parent) -> uint32;
		auto Refit(uint32 NodeIndex) -> void;
		static auto IsAdmissible(const FViewportPickingMutation& Mutation) -> bool;
		static auto MakeFatBounds(const FBox& Exact) -> FBox;

		TWeakObjectPtr<DLevel> Level;
		uint64 Subscription = 0;
		uint64 AppliedRevision = 0;
		std::vector<FViewportPickingMutationBatch> PendingBatches;
		std::unordered_map<uint64, FLeaf> Leaves;
		std::vector<FNode> Nodes;
		uint32 Root = std::numeric_limits<uint32>::max();
		bool bComplete = false;
		bool bNeedsRebuild = false;
		FViewportPickingSceneIndexDiagnostics Diagnostics;
	};
}
