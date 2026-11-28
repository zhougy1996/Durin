#include "Viewport/TransformProxy.h"
#include "Components/SceneComponent.h"

#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Math/Operations.h"
#include "Math/TransformDecomposition.h"
#include "Misc/ITransaction.h"

namespace Durin
{
	using namespace LevelEditor;
	namespace
	{
		constexpr double Epsilon = 1.e-8;

		auto SameTransform(const FTransform& A, const FTransform& B) -> bool
		{
			return Math::Length(A.Translation - B.Translation) <= Epsilon
				&& Math::Length(A.Scale3D - B.Scale3D) <= Epsilon
				&& 1.0 - std::abs(Math::Dot(Math::Normalize(A.Rotation), Math::Normalize(B.Rotation))) <= Epsilon;
		}
		auto TrySetTransform(const FTransformComponentBinding& Target, const FTransform& Value, FTransformGizmoWriteContext Context) -> bool
		{
			try { return Target.SetTransform(Value, Context); }
			catch (...) { return false; }
		}
		struct FFrameMapping
		{
			FTransform Basis;
			std::optional<FMatrix> Scale;

			FFrameMapping(const FTransform& Frame, const FVector3& ReferenceScale) : Basis(Frame)
			{
				Basis.Scale3D = FVector3(1.0);
				if (Frame.Scale3D != ReferenceScale) Scale = Math::ScaleMatrix(Frame.Scale3D / ReferenceScale);
			}

			bool ToWorld(const FTransform& Relative, FTransform& World) const
			{
				auto Local = Relative;
				if (Scale && !TryMakeTransformFromMatrix(*Scale * Relative.ToMatrix(), Local)) return false;
				World = FTransform::Combine(Basis, Local);
				return true;
			}
		};
	}

	// The frames use the selection binding that was current during this gesture.
	class FTransformProxyChange final : public FCommandChange
	{
	public:
		struct FTopologyCheck
		{
			FTopologyRevisionReader ReadRevision;
			uint64 Revision;
		};
		FTransformProxyChange(DTransformProxy& Proxy, uint64 InHistoryId, std::string InDescription)
			: From(Proxy.GetTransform()), To(From), HistoryId(InHistoryId), ProxyKey(&Proxy), Description(std::move(InDescription)) {}

		void RegisterTarget(const DTransformProxy& Proxy, size_t Index)
		{
			if (Index < RegisteredCount) return;
			const auto& Bound = Proxy.Targets[Index];
			if (auto State = Proxy.EditStates.find(Index); State != Proxy.EditStates.end()) EditStates.emplace(*State);
			if (const auto Reader = Bound.Target->GetTopologyRevisionReader())
			{
				if (!TopologyChecks.contains(Bound.Owner))
				{
					auto* Owner = Bound.Owner.ResolveObjectPtr();
					if (!Owner) throw std::runtime_error("Transform data owner is unavailable.");
					TopologyChecks.emplace(Bound.Owner, FTopologyCheck{Reader, Reader(*Owner, HistoryId)});
				}
			}
			else ObjectBindings.push_back(Index);
			++RegisteredCount;
		}
		FTransform From, To;
		auto GetDescription() const -> std::string_view override { return Description; }
		auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }

		auto Apply(DObject* Owner) -> void override { Replay(*static_cast<DTransformProxy*>(Owner), To); }
		auto Revert(DObject* Owner) -> void override { Replay(*static_cast<DTransformProxy*>(Owner), From); }
		auto AddReferencedObjects(FReferenceCollector& Collector) -> void override
		{
			// Transaction package inspection visits direct references only.
			if (RegisteredCount)
				if (auto* Proxy = Cast<DTransformProxy>(ProxyKey.ResolveObjectPtr())) Proxy->CollectTargetReferences(Collector);
		}

	private:
		void Replay(DTransformProxy& Proxy, const FTransform& Frame) const
		{
			if (RegisteredCount)
			{
				if (!Proxy.SetTransform(Frame, EditStates, HistoryId)) throw std::runtime_error("Could not replay the bound transform targets.");
			}
			else Proxy.SharedFrame = Frame; // Empty gestures have no component writes to replay.
			Proxy.bObserveSelection = false;
			if (auto ActiveTarget = Proxy.ActiveSelection.lock()) *ActiveTarget = &Proxy;
		}

		auto HasExpired(const DObject* Owner) const noexcept -> bool override
		{
			const auto* Proxy = Cast<DTransformProxy>(Owner);
			return !Proxy
				|| !std::ranges::all_of(TopologyChecks, [this](const auto& Pair) {
					auto* Component = Pair.first.ResolveObjectPtr();
					return Component && Pair.second.ReadRevision(*Component, HistoryId) == Pair.second.Revision;
				})
				|| !std::ranges::all_of(ObjectBindings, [Proxy](size_t Index) {
					const auto& Bound = Proxy->Targets[Index];
					return Bound.Target->IsValid() && FObjectKey(Bound.Target->GetDataOwner()) == Bound.Owner;
				});
		}

		uint64 HistoryId;
		FObjectKey ProxyKey;
		size_t RegisteredCount = 0;
		std::vector<size_t> ObjectBindings;
		std::string Description;
		DTransformProxy::FEditStates EditStates;
		std::unordered_map<FObjectKey, FTopologyCheck, FObjectKeyHash> TopologyChecks;
	};

	FTransformGizmoTargetSet DTransformProxy::GetSelection() const
	{
		FTransformGizmoTargetSet Selection;
		Selection.CollectionLabel = CollectionLabel;
		for (const auto& Bound : Targets) Selection.Targets.push_back(Bound.Target);
		return Selection;
	}

	void DTransformProxy::AddComponent(DSceneComponent* Component)
	{
		auto Selection = GetSelection();
		Selection.AddComponent(Component);
		RebuildBinding(Selection);
	}

	void DTransformProxy::AddComponentCustom(DSceneComponent* Component, FComponentTransformGetter Getter,
		std::function<bool(const FTransform&)> Setter)
	{
		if (!Setter) throw std::invalid_argument("Missing custom transform setter.");
		AddComponentCustom(Component, std::move(Getter), [Write = std::move(Setter)](const FTransform& Value, FTransformGizmoWriteContext) {
			return Write(Value);
		});
	}

	void DTransformProxy::AddComponentCustom(DSceneComponent* Component, FComponentTransformGetter Getter,
		FComponentTransformSetter Setter, FTransformComponentOptions Options)
	{
		auto Selection = GetSelection();
		Selection.AddComponentCustom(Component, std::move(Getter), std::move(Setter), std::move(Options));
		RebuildBinding(Selection);
	}

	void DTransformProxy::RebuildBinding(const FTransformGizmoTargetSet& Selection)
	{
		FVector3 Center(0.0);
		for (const auto& Binding : Selection.Targets)
		{
			if (!Binding->IsValid()) throw std::invalid_argument("Invalid transform component.");
			Center += Binding->GetTransform().Translation;
		}
		Initialize(Selection, Center / static_cast<double>(Selection.Targets.size()), Selection.Targets.front()->GetTransform().Rotation);
	}

	auto DTransformProxy::Initialize(const FTransformGizmoTargetSet& Selection, const FVector3& Pivot, const FQuat& Basis, const FVector3& Scale) -> void
	{
		if (bSealed) throw std::logic_error("A bound transform proxy cannot be rebound.");
		FTransform Frame;
		Frame.Translation = Pivot;
		Frame.Rotation = Basis;
		std::vector<FBoundTarget> BoundTargets;
		BoundTargets.reserve(Selection.Targets.size());
		for (const auto& Target : Selection.Targets)
		{
			if (!Target || !Target->IsValid()) throw std::invalid_argument("Invalid transform binding.");
			BoundTargets.push_back({Target, Target->GetIdentity(), FObjectKey(Target->GetDataOwner()),
				FTransform::MakeRelative(Target->GetTransform(), Frame)});
		}
		CollectionLabel = Selection.CollectionLabel;
		Targets = std::move(BoundTargets);
		SharedFrame = Frame;
		ReferenceScale = SharedFrame.Scale3D = Scale;
	}

	void DTransformProxy::Activate(const std::shared_ptr<TStrongObjectPtr<DTransformProxy>>& ActiveTarget)
	{
		bSealed = true;
		ActiveSelection = ActiveTarget;
		*ActiveTarget = this;
	}

	void DTransformProxy::SyncSelectionFrame(const FVector3& Center, const FQuat& Rotation)
	{
		// Follow edits made outside the gizmo. After history replay the restored frame takes precedence.
		if (bObserveSelection)
		{
			SharedFrame.Translation += Center - LastCenter;
			if (Rotation != LastRotation)
				SharedFrame.Rotation = Math::Normalize(Rotation * Math::Inverse(LastRotation) * SharedFrame.Rotation);
		}
		LastCenter = Center;
		LastRotation = Rotation;
		bObserveSelection = true;
	}

	bool DTransformProxy::MatchesTargetTransforms() const
	{
		const FFrameMapping Mapping(SharedFrame, ReferenceScale);
		return std::ranges::all_of(Targets, [&](const auto& Bound) {
			FTransform Expected;
			return Bound.Target->IsValid() && FObjectKey(Bound.Target->GetDataOwner()) == Bound.Owner
				&& Mapping.ToWorld(Bound.Relative, Expected) && SameTransform(Expected, Bound.Target->GetTransform());
		});
	}

	void DTransformProxy::BeginTransformEditSequence()
	{
		if (Targets.empty()) throw std::logic_error("Cannot begin an empty transform edit sequence.");
		bSealed = true;
		std::vector<FTransform> Baselines;
		std::vector<TObjectPtr<DPackage>> AffectedPackages;
		FEditStates States;
		for (const auto& Bound : Targets)
		{
			if (!Bound.Target->IsValid()) throw std::runtime_error("Transform binding expired before the edit sequence.");
			if (auto State = Bound.Target->CaptureEditState()) States.emplace(Baselines.size(), std::move(State));
			Baselines.push_back(Bound.Target->GetTransform());
			if (auto* Package = Bound.Target->GetPackage(); Package && !std::ranges::contains(AffectedPackages, Package, &TObjectPtr<DPackage>::Get))
				AffectedPackages.emplace_back(Package);
		}
		InitialFrame = SharedFrame;
		InitialTransforms = std::move(Baselines);
		Packages = std::move(AffectedPackages);
		EditStates = std::move(States);
	}

	void DTransformProxy::CollectTargetReferences(FReferenceCollector& Collector) const
	{
		for (const auto& Bound : Targets) Bound.Target->AddReferencedObjects(Collector);
	}

	void DTransformProxy::AddReferencedObjects(FReferenceCollector& Collector)
	{
		Super::AddReferencedObjects(Collector);
		CollectTargetReferences(Collector);
	}

	auto DTransformProxy::SetTransform(const FTransform& Frame) -> bool
	{
		return SetTransform(Frame, EditStates, 0);
	}

	bool DTransformProxy::SetTransform(const FTransform& Frame, const FEditStates& States, uint64 ReplayHistoryId)
	{
		if (Targets.empty()) return false;
		const FFrameMapping Mapping(Frame, ReferenceScale);
		auto* const Recording = GUndo;
		const auto Operation = Recording ? Recording->GetContext().OperationId : 0;
		const auto RecordingIsCurrent = [&] { return GUndo == Recording && (!Recording || Recording->GetContext().OperationId == Operation); };
		for (size_t Index = 0; Index < Targets.size(); ++Index)
		{
			const auto& Bound = Targets[Index];
			auto* Owner = Bound.Target->GetDataOwner();
			if (!Bound.Target->IsValid(ReplayHistoryId) || FObjectKey(Owner) != Bound.Owner || !RecordingIsCurrent()) return false;
			FTransform World;
			if (!Mapping.ToWorld(Bound.Relative, World)) return false;
			if (Owner)
			{
				Owner->Modify();
				if (!RecordingIsCurrent() || !Bound.Target->IsValid(ReplayHistoryId) || Bound.Target->GetDataOwner() != Owner) return false;
			}
			if (RecordingChange)
			{
				RecordingChange->RegisterTarget(*this, Index);
				if (!RecordingIsCurrent()) return false;
			}
			const auto State = States.find(Index);
			if (!TrySetTransform(*Bound.Target, World, {State == States.end() ? nullptr : State->second.get(), ReplayHistoryId}) || !RecordingIsCurrent()) return false;
		}
		SharedFrame = Frame;
		return true;
	}

	auto DTransformProxy::MatchesSelection(const FTransformGizmoTargetSet& Selection) const -> bool
	{
		if (Targets.empty() || Selection.Targets.size() != Targets.size()) return false;
		for (size_t Index = 0; Index < Targets.size(); ++Index)
		{
			const auto& Target = Selection.Targets[Index];
			if (!Target || !Target->IsValid()) return false;
			const auto Matches = [&](const FBoundTarget& Bound) {
				return Bound.Identity == Target->GetIdentity() && Bound.Owner == FObjectKey(Target->GetDataOwner());
			};
			if (!Matches(Targets[Index]) && (Index == 0 || !std::ranges::any_of(Targets, Matches))) return false;
		}
		return true;
	}

	auto DTransformProxy::Restore() -> bool
	{
		bool bRestored = true;
		for (size_t Index = 0; Index < InitialTransforms.size(); ++Index)
		{
			const auto& Snapshot = Targets[Index];
			if (!Snapshot.Target->IsValid() || FObjectKey(Snapshot.Target->GetDataOwner()) != Snapshot.Owner) continue;
			const auto State = EditStates.find(Index);
			if (!TrySetTransform(*Snapshot.Target, InitialTransforms[Index], {State == EditStates.end() ? nullptr : State->second.get(), 0})) bRestored = false;
		}
		for (size_t Index = 0; Index < InitialTransforms.size(); ++Index)
		{
			const auto& Snapshot = Targets[Index];
			if (Snapshot.Target->IsValid() && FObjectKey(Snapshot.Target->GetDataOwner()) == Snapshot.Owner
				&& !SameTransform(Snapshot.Target->GetTransform(), InitialTransforms[Index])) bRestored = false;
		}
		if (!bRestored)
		{
			for (const auto& Package : Packages)
				if (Package) Package->MarkDirty();
		}
		if (bRestored) SharedFrame = InitialFrame;
		return bRestored;
	}

	auto DTransformProxy::DescribeChange(std::string_view Action) const -> std::string
	{
		return Targets.size() == 1 ? std::format("{} '{}'", Action, Targets.front().Target->GetLabel())
			: std::format("{} {} {}", Action, Targets.size(), CollectionLabel);
	}

	FTransformProxyChangeSource::FTransformProxyChangeSource(DTransformProxy& InProxy, uint64 HistoryId, std::string Description)
		: Proxy(InProxy), Change(std::make_unique<FTransformProxyChange>(Proxy, HistoryId, std::move(Description)))
	{
		Proxy.RecordingChange = Change.get();
	}

	FTransformProxyChangeSource::~FTransformProxyChangeSource()
	{
		Proxy.RecordingChange = nullptr;
	}

	auto FTransformProxyChangeSource::EndChange() -> std::unique_ptr<FCommandChange>
	{
		Change->To = Proxy.GetTransform();
		Proxy.RecordingChange = nullptr;
		return std::move(Change);
	}

	auto DTransformProxy::EndTransformEditSequence() -> void
	{
		bObserveSelection = false;
		InitialTransforms.clear();
		EditStates.clear();
		Packages.clear();
	}
}
