#include "Viewport/ViewportPickingSceneIndex.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/Actor.h"

namespace Durin::Editor::Level
{
	namespace
	{
		constexpr double kRayEpsilon = 1.e-8;
		constexpr double kFatBoundsScale = 0.1;
		constexpr double kMinimumFatMargin = 0.01;
		constexpr uint64 kSceneMemoryBudget = 64ull * 1024ull * 1024ull;
		constexpr uint64 kMaximumBytesPerPrimitive = 384;
		constexpr uint32 kInvalidNode = std::numeric_limits<uint32>::max();

		auto UnionBounds(const FBox& A, const FBox& B) -> FBox
		{
			if (!A.bIsValid) return B;
			if (!B.bIsValid) return A;
			return {Math::Min(A.Min, B.Min), Math::Max(A.Max, B.Max)};
		}

		auto Contains(const FBox& Outer, const FBox& Inner) -> bool
		{
			return Outer.bIsValid && Inner.bIsValid
				&& Inner.Min.x >= Outer.Min.x && Inner.Min.y >= Outer.Min.y && Inner.Min.z >= Outer.Min.z
				&& Inner.Max.x <= Outer.Max.x && Inner.Max.y <= Outer.Max.y && Inner.Max.z <= Outer.Max.z;
		}

		auto IntersectRayBox(const FVector3& Origin, const FVector3& Direction, const FBox& Box) -> bool
		{
			if (!Box.bIsValid || !Math::IsFinite(Box.Min) || !Math::IsFinite(Box.Max)) return false;
			double NearDistance = 0.0;
			double FarDistance = std::numeric_limits<double>::max();
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				if (std::abs(Direction[Axis]) <= kRayEpsilon)
				{
					if (Origin[Axis] < Box.Min[Axis] || Origin[Axis] > Box.Max[Axis]) return false;
					continue;
				}
				double A = (Box.Min[Axis] - Origin[Axis]) / Direction[Axis];
				double B = (Box.Max[Axis] - Origin[Axis]) / Direction[Axis];
				if (A > B) std::swap(A, B);
				NearDistance = std::max(NearDistance, A);
				FarDistance = std::min(FarDistance, B);
				if (NearDistance > FarDistance) return false;
			}
			return FarDistance >= 0.0;
		}
	}

	FViewportPickingSceneIndex::FViewportPickingSceneIndex() = default;
	FViewportPickingSceneIndex::~FViewportPickingSceneIndex() { Retire(); }

	auto FViewportPickingSceneIndex::Retire() -> void
	{
		if (DLevel* Current = Level.Get(); Current && Subscription)
			Current->UnsubscribeEditorPickingPrimitives(Subscription);
		Level = nullptr;
		Subscription = 0;
		AppliedRevision = 0;
		PendingBatches.clear();
		Leaves.clear();
		Nodes.clear();
		Root = kInvalidNode;
		bComplete = false;
		bNeedsRebuild = false;
		Diagnostics.RetainedBytes = 0;
	}

	auto FViewportPickingSceneIndex::SetLevel(DLevel* InLevel) -> void
	{
		if (Level.Get() == InLevel) return;
		Retire();
		Level = InLevel;
		if (!InLevel) return;
		const std::weak_ptr<FViewportPickingSceneIndex> WeakThis = weak_from_this();
		Subscription = InLevel->SubscribeEditorPickingPrimitives(
			[WeakThis](const FEditorPickingPrimitiveMutationBatch& Batch)
			{
				if (const std::shared_ptr<FViewportPickingSceneIndex> Index = WeakThis.lock())
					Index->ReceiveBatch(Batch);
			});
		if (!Subscription) Retire();
	}

	auto FViewportPickingSceneIndex::ReceiveBatch(const FEditorPickingPrimitiveMutationBatch& Batch) -> void
	{
		PendingBatches.push_back(Batch);
	}

	auto FViewportPickingSceneIndex::IsAdmissible(const FEditorPickingPrimitiveMutation& Mutation) -> bool
	{
		return !Mutation.bRetired && Mutation.bVisible && Mutation.Actor.Get() && Mutation.Component.Get()
			&& Mutation.PrimitiveId != InvalidPrimitiveSceneId
			&& Mutation.Family != EEditorPickingPrimitiveFamily::Unsupported
			&& Mutation.WorldBounds.bIsValid && Math::IsFinite(Mutation.WorldBounds.Min)
			&& Math::IsFinite(Mutation.WorldBounds.Max);
	}

	auto FViewportPickingSceneIndex::MakeFatBounds(const FBox& Exact) -> FBox
	{
		const FVector3 Margin = Math::Max(Exact.GetExtent() * kFatBoundsScale, FVector3(kMinimumFatMargin));
		return {Exact.Min - Margin, Exact.Max + Margin};
	}

	auto FViewportPickingSceneIndex::ApplySnapshot(const FEditorPickingPrimitiveMutationBatch& Batch) -> bool
	{
		Leaves.clear();
		for (const FEditorPickingPrimitiveMutation& Mutation : Batch.Mutations)
		{
			if (!IsAdmissible(Mutation)) continue;
			FLeaf Leaf;
			Leaf.Candidate = {Mutation.PrimitiveId, Mutation.Actor, Mutation.Component,
				Mutation.PrimitiveId.Value, Mutation.RegistrationGeneration, Mutation.Family};
			Leaf.ExactBounds = Mutation.WorldBounds;
			Leaf.FatBounds = MakeFatBounds(Mutation.WorldBounds);
			Leaves.insert_or_assign(Mutation.PrimitiveId.Value, std::move(Leaf));
		}
		AppliedRevision = Batch.Revision;
		bComplete = true;
		bNeedsRebuild = true;
		++Diagnostics.SnapshotBuilds;
		return true;
	}

	auto FViewportPickingSceneIndex::ApplyMutation(const FEditorPickingPrimitiveMutation& Mutation) -> bool
	{
		++Diagnostics.Mutations;
		if (!IsAdmissible(Mutation))
		{
			if (Leaves.erase(Mutation.PrimitiveId.Value) > 0) bNeedsRebuild = true;
			return true;
		}
		const auto Existing = Leaves.find(Mutation.PrimitiveId.Value);
		if (Existing == Leaves.end())
		{
			FLeaf Leaf;
			Leaf.Candidate = {Mutation.PrimitiveId, Mutation.Actor, Mutation.Component,
				Mutation.PrimitiveId.Value, Mutation.RegistrationGeneration, Mutation.Family};
			Leaf.ExactBounds = Mutation.WorldBounds;
			Leaf.FatBounds = MakeFatBounds(Mutation.WorldBounds);
			Leaves.emplace(Mutation.PrimitiveId.Value, std::move(Leaf));
			bNeedsRebuild = true;
			return true;
		}
		FLeaf& Leaf = Existing->second;
		Leaf.Candidate = {Mutation.PrimitiveId, Mutation.Actor, Mutation.Component,
			Mutation.PrimitiveId.Value, Mutation.RegistrationGeneration, Mutation.Family};
		Leaf.ExactBounds = Mutation.WorldBounds;
		if (!Contains(Leaf.FatBounds, Mutation.WorldBounds))
		{
			Leaf.FatBounds = MakeFatBounds(Mutation.WorldBounds);
			bNeedsRebuild = true;
			++Diagnostics.Reinsertions;
		}
		return true;
	}

	auto FViewportPickingSceneIndex::Synchronize() -> bool
	{
		DLevel* Current = Level.Get();
		if (!Current || !Subscription) return false;
		for (const FEditorPickingPrimitiveMutationBatch& Batch : PendingBatches)
		{
			if (Batch.bCompleteSnapshot)
			{
				if (!ApplySnapshot(Batch)) return false;
				continue;
			}
			if (!bComplete || Batch.Revision != AppliedRevision + 1)
			{
				ApplySnapshot(Current->CaptureEditorPickingPrimitiveSnapshot());
				break;
			}
			for (const FEditorPickingPrimitiveMutation& Mutation : Batch.Mutations)
				if (!ApplyMutation(Mutation)) return false;
			AppliedRevision = Batch.Revision;
		}
		PendingBatches.clear();
		if (bNeedsRebuild && !Rebuild())
		{
			bComplete = false;
			return false;
		}
		return bComplete;
	}

	auto FViewportPickingSceneIndex::BuildRange(
		std::vector<FPrimitiveSceneId>& Ids, size_t Begin, size_t End, uint32 Parent) -> uint32
	{
		const uint32 NodeIndex = static_cast<uint32>(Nodes.size());
		Nodes.push_back({});
		Nodes[NodeIndex].Parent = Parent;
		FBox Bounds;
		FBox Centroids;
		for (size_t Index = Begin; Index < End; ++Index)
		{
			const FLeaf& Leaf = Leaves.at(Ids[Index].Value);
			Bounds = UnionBounds(Bounds, Leaf.FatBounds);
			Centroids.AddPoint(Leaf.FatBounds.GetCenter());
		}
		Nodes[NodeIndex].Bounds = Bounds;
		if (End - Begin == 1)
		{
			Nodes[NodeIndex].LeafId = Ids[Begin];
			Leaves.at(Ids[Begin].Value).NodeIndex = NodeIndex;
			return NodeIndex;
		}
		const FVector3 Extent = Centroids.Max - Centroids.Min;
		uint32 Axis = Extent.y > Extent.x ? 1u : 0u;
		if (Extent.z > Extent[Axis]) Axis = 2u;
		std::stable_sort(Ids.begin() + Begin, Ids.begin() + End,
			[this, Axis](FPrimitiveSceneId A, FPrimitiveSceneId B)
			{
				const double CA = Leaves.at(A.Value).FatBounds.GetCenter()[Axis];
				const double CB = Leaves.at(B.Value).FatBounds.GetCenter()[Axis];
				return CA < CB || (CA == CB && A.Value < B.Value);
			});
		const size_t Middle = Begin + (End - Begin) / 2;
		const uint32 Left = BuildRange(Ids, Begin, Middle, NodeIndex);
		const uint32 Right = BuildRange(Ids, Middle, End, NodeIndex);
		Nodes[NodeIndex].Left = Left;
		Nodes[NodeIndex].Right = Right;
		return NodeIndex;
	}

	auto FViewportPickingSceneIndex::Rebuild() -> bool
	{
		const auto BuildStart = std::chrono::steady_clock::now();
		Nodes.clear();
		Root = kInvalidNode;
		const uint64 EstimatedBytes = Leaves.size() * sizeof(FLeaf)
			+ (Leaves.empty() ? 0 : (Leaves.size() * 2 - 1) * sizeof(FNode));
		Diagnostics.RetainedBytes = EstimatedBytes;
		if (EstimatedBytes > kSceneMemoryBudget
			|| (!Leaves.empty() && EstimatedBytes / Leaves.size() > kMaximumBytesPerPrimitive)) return false;
		if (!Leaves.empty())
		{
			std::vector<FPrimitiveSceneId> Ids;
			Ids.reserve(Leaves.size());
			for (const auto& [Id, Leaf] : Leaves)
			{
				(void)Leaf;
				Ids.push_back(FPrimitiveSceneId(Id));
			}
			std::ranges::sort(Ids, {}, &FPrimitiveSceneId::Value);
			Nodes.reserve(Ids.size() * 2 - 1);
			Root = BuildRange(Ids, 0, Ids.size(), kInvalidNode);
		}
		bNeedsRebuild = false;
		++Diagnostics.Rebuilds;
		Diagnostics.BuildNanoseconds += static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - BuildStart).count());
		return true;
	}

	auto FViewportPickingSceneIndex::Refit(uint32 NodeIndex) -> void
	{
		while (NodeIndex != kInvalidNode)
		{
			FNode& Node = Nodes[NodeIndex];
			if (Node.LeafId != InvalidPrimitiveSceneId) Node.Bounds = Leaves.at(Node.LeafId.Value).FatBounds;
			else Node.Bounds = UnionBounds(Nodes[Node.Left].Bounds, Nodes[Node.Right].Bounds);
			NodeIndex = Node.Parent;
		}
	}

	auto FViewportPickingSceneIndex::QueryRay(const FVector3& Origin, const FVector3& Direction,
		std::vector<FViewportPickingSceneCandidate>& OutCandidates) -> bool
	{
		OutCandidates.clear();
		if (!Math::IsFinite(Origin) || !Math::IsFinite(Direction) || !Synchronize())
		{
			++Diagnostics.ReferenceFallbacks;
			return false;
		}
		if (Root == kInvalidNode) return true;
		std::vector<uint32> Stack{Root};
		while (!Stack.empty())
		{
			const uint32 NodeIndex = Stack.back();
			Stack.pop_back();
			const FNode& Node = Nodes[NodeIndex];
			++Diagnostics.NodeVisits;
			++Diagnostics.BoundsTests;
			if (!IntersectRayBox(Origin, Direction, Node.Bounds)) continue;
			if (Node.LeafId != InvalidPrimitiveSceneId)
			{
				const FLeaf& Leaf = Leaves.at(Node.LeafId.Value);
				++Diagnostics.BoundsTests;
				if (IntersectRayBox(Origin, Direction, Leaf.ExactBounds)) OutCandidates.push_back(Leaf.Candidate);
			}
			else
			{
				Stack.push_back(Node.Right);
				Stack.push_back(Node.Left);
			}
		}
		std::ranges::sort(OutCandidates, {}, &FViewportPickingSceneCandidate::StableTieKey);
		Diagnostics.CandidatePrimitives += OutCandidates.size();
		return true;
	}
}
