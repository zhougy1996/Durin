#pragma once

#include "DObject/ObjectPtr.h"
#include "Engine/Level.h"

namespace Durin
{
	class AActor;
	class DPrimitiveComponent;

	struct FViewportPickingSceneCandidate
	{
		FPrimitiveSceneId PrimitiveId = InvalidPrimitiveSceneId;
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DPrimitiveComponent> Component;
		uint64 StableTieKey = 0;
		uint64 RegistrationGeneration = 0;
		EEditorPickingPrimitiveFamily Family = EEditorPickingPrimitiveFamily::Unsupported;
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
		auto GetLevel() const -> DLevel* { return Level.Get(); }

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
			FPrimitiveSceneId LeafId = InvalidPrimitiveSceneId;
		};

		auto Retire() -> void;
		auto ReceiveBatch(const FEditorPickingPrimitiveMutationBatch& Batch) -> void;
		auto Synchronize() -> bool;
		auto ApplySnapshot(const FEditorPickingPrimitiveMutationBatch& Batch) -> bool;
		auto ApplyMutation(const FEditorPickingPrimitiveMutation& Mutation) -> bool;
		auto Rebuild() -> bool;
		auto BuildRange(std::vector<FPrimitiveSceneId>& Ids, size_t Begin, size_t End, uint32 Parent) -> uint32;
		auto Refit(uint32 NodeIndex) -> void;
		static auto IsAdmissible(const FEditorPickingPrimitiveMutation& Mutation) -> bool;
		static auto MakeFatBounds(const FBox& Exact) -> FBox;

		TWeakObjectPtr<DLevel> Level;
		uint64 Subscription = 0;
		uint64 AppliedRevision = 0;
		std::vector<FEditorPickingPrimitiveMutationBatch> PendingBatches;
		std::unordered_map<uint64, FLeaf> Leaves;
		std::vector<FNode> Nodes;
		uint32 Root = std::numeric_limits<uint32>::max();
		bool bComplete = false;
		bool bNeedsRebuild = false;
		FViewportPickingSceneIndexDiagnostics Diagnostics;
	};
}
