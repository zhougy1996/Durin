#include "Viewport/TransformProxy.h"

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
		auto TrySetTransform(ITransformGizmoTarget& Target, const FTransform& Value) -> bool
		{
			try { return Target.SetTransform(Value); }
			catch (...) { return false; }
		}
	}

	// Whole-object snapshots restore the targets. The command restores the shared frame.
	class FTransformProxyChange final : public FCommandChange
	{
	public:
		struct FBinding
		{
			std::shared_ptr<ITransformGizmoTarget> Target;
			FObjectKey Owner;

			auto IsValid() const -> bool { return Target->IsValid() && FObjectKey(Target->GetDataOwner()) == Owner; }
		};
		struct FTopologyCheck
		{
			ITransformGizmoTarget::FTopologyRevisionReader ReadRevision;
			uint64 Revision;
		};
		FTransformProxyChange(const FTransform& Initial, uint64 InHistoryId, std::string InDescription)
			: From(Initial), To(Initial), HistoryId(InHistoryId), Description(std::move(InDescription)) {}

		void RegisterTarget(const std::shared_ptr<ITransformGizmoTarget>& Target, DObject& Owner)
		{
			const FObjectKey Key(&Owner);
			if (const auto Reader = Target->GetTopologyRevisionReader())
			{
				if (!TopologyChecks.contains(Key))
					TopologyChecks.emplace(Key, FTopologyCheck{Reader, Reader(Owner, HistoryId)});
			}
			else SnapshotBindings.push_back({Target, Key});
		}
		FTransform From, To;
		auto GetDescription() const -> std::string_view override { return Description; }
		auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }

		auto Apply(DObject* Owner) -> void override { static_cast<DTransformProxy*>(Owner)->SetHistoryFrame(To); }
		auto Revert(DObject* Owner) -> void override { static_cast<DTransformProxy*>(Owner)->SetHistoryFrame(From); }
		auto AddReferencedObjects(FReferenceCollector& Collector) -> void override
		{
			for (const auto& Binding : SnapshotBindings) Binding.Target->AddReferencedObjects(Collector);
		}

	private:
		auto HasExpired(const DObject* Owner) const noexcept -> bool override
		{
			return !Cast<DTransformProxy>(Owner)
				|| !std::ranges::all_of(TopologyChecks, [this](const auto& Pair) {
					auto* Component = Pair.first.ResolveObjectPtr();
					return Component && Pair.second.ReadRevision(*Component, HistoryId) == Pair.second.Revision;
				})
				|| !std::ranges::all_of(SnapshotBindings, &FBinding::IsValid);
		}

		uint64 HistoryId;
		std::string Description;
		std::vector<FBinding> SnapshotBindings;
		std::unordered_map<FObjectKey, FTopologyCheck, FObjectKeyHash> TopologyChecks;
	};

	auto DTransformProxy::Initialize(const FTransformGizmoTargetSet& Targets, const FVector3& Pivot, const FQuat& Basis) -> void
	{
		CollectionLabel = Targets.CollectionLabel;
		SharedFrame.Translation = Pivot;
		SharedFrame.Rotation = Basis;
		for (const auto& Target : Targets.Targets)
			if (Target && Target->IsValid()) Selection.push_back({Target->GetIdentity(), FObjectKey(Target->GetDataOwner())});
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

	void DTransformProxy::BeginTransformEditSequence(const FTransformGizmoTargetSet& Targets)
	{
		if (!MatchesSelection(Targets)) throw std::runtime_error("Transform selection changed before the edit sequence.");
		std::vector<FTargetSnapshot> NewSnapshots;
		std::vector<TObjectPtr<DPackage>> NewPackages;
		NewSnapshots.reserve(Targets.Targets.size());
		for (const auto& Target : Targets.Targets)
		{
			Target->BeginTransformEditSequence();
			NewSnapshots.push_back({Target, FObjectKey(Target->GetDataOwner()), Target->GetTransform()});
			if (DPackage* Package = Target->GetPackage(); Package && !std::ranges::contains(NewPackages, Package, &TObjectPtr<DPackage>::Get))
				NewPackages.emplace_back(Package);
		}
		InitialFrame = SharedFrame;
		Snapshots = std::move(NewSnapshots);
		Packages = std::move(NewPackages);
	}

	auto DTransformProxy::SetTransform(const FTransform& Frame) -> bool
	{
		const FVector3 Offset = Frame.Translation - InitialFrame.Translation;
		const bool bRotate = Frame.Rotation != InitialFrame.Rotation;
		const bool bScale = Frame.Scale3D != InitialFrame.Scale3D;
		const FQuat Rotation = Frame.Rotation * Math::Inverse(InitialFrame.Rotation);
		const auto& Pivot = InitialFrame.Translation;
		FMatrix Scale(1.0);
		if (bScale)
			Scale = Math::TranslationMatrix(Pivot) * Math::RotationMatrix(InitialFrame.Rotation)
				* Math::ScaleMatrix(Frame.Scale3D / InitialFrame.Scale3D) * Math::RotationMatrix(Math::Inverse(InitialFrame.Rotation))
				* Math::TranslationMatrix(-Pivot);

		auto* const Recording = GUndo;
		const auto Operation = Recording ? Recording->GetContext().OperationId : 0;
		const auto RecordingIsCurrent = [&] { return GUndo == Recording && (!Recording || Recording->GetContext().OperationId == Operation); };
		for (auto& Snapshot : Snapshots)
		{
			auto* Owner = Snapshot.Target->GetDataOwner();
			if (!Snapshot.Target->IsValid() || FObjectKey(Owner) != Snapshot.Owner || !RecordingIsCurrent()) return false;
			FTransform Transform = Snapshot.Initial;
			if (bScale && !TryMakeTransformFromMatrix(Scale * Snapshot.Initial.ToMatrix(), Transform)) return false;
			if (bRotate)
			{
				Transform.Translation = Pivot + Rotation * (Transform.Translation - Pivot);
				Transform.Rotation = Math::Normalize(Rotation * Transform.Rotation);
			}
			Transform.Translation += Offset;
			if (Owner && ModifiedOwners.insert(Snapshot.Owner).second)
			{
				Owner->Modify();
				if (!RecordingIsCurrent() || !Snapshot.Target->IsValid() || Snapshot.Target->GetDataOwner() != Owner)
					return false;
			}
			if (Recording && !Recording->ContainsObject(Owner, true)) return false;
			if (RecordingChange && !Snapshot.bRegistered)
			{
				RecordingChange->RegisterTarget(Snapshot.Target, *Owner);
				Snapshot.bRegistered = true;
				if (!RecordingIsCurrent()) return false;
			}
			if (!TrySetTransform(*Snapshot.Target, Transform) || !RecordingIsCurrent()) return false;
		}
		SharedFrame = Frame;
		return true;
	}

	auto DTransformProxy::MatchesSelection(const FTransformGizmoTargetSet& Targets) const -> bool
	{
		if (Selection.empty() || Targets.Targets.size() != Selection.size()) return false;
		for (size_t Index = 0; Index < Selection.size(); ++Index)
		{
			const auto& Target = Targets.Targets[Index];
			if (!Target || !Target->IsValid()) return false;
			const FSelectionBinding Binding{Target->GetIdentity(), FObjectKey(Target->GetDataOwner())};
			// The first target supplies the local basis; the remaining members are unordered.
			if (Binding != Selection[Index] && (Index == 0 || !std::ranges::contains(Selection, Binding))) return false;
		}
		return true;
	}

	auto DTransformProxy::Restore() -> bool
	{
		bool bRestored = true;
		for (const auto& Snapshot : Snapshots)
		{
			if (!Snapshot.Target->IsValid() || FObjectKey(Snapshot.Target->GetDataOwner()) != Snapshot.Owner) continue;
			if (!TrySetTransform(*Snapshot.Target, Snapshot.Initial)) bRestored = false;
		}
		for (const auto& Snapshot : Snapshots)
		{
			if (Snapshot.Target->IsValid() && FObjectKey(Snapshot.Target->GetDataOwner()) == Snapshot.Owner
				&& !SameTransform(Snapshot.Target->GetTransform(), Snapshot.Initial)) bRestored = false;
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
		return Snapshots.size() == 1 ? std::format("{} '{}'", Action, Snapshots.front().Target->GetLabel())
			: std::format("{} {} {}", Action, Snapshots.size(), CollectionLabel);
	}

	FTransformProxyChangeSource::FTransformProxyChangeSource(DTransformProxy& InProxy, uint64 HistoryId, std::string Description)
		: Proxy(InProxy), Change(std::make_unique<FTransformProxyChange>(Proxy.GetTransform(), HistoryId, std::move(Description)))
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
		Snapshots.clear();
		ModifiedOwners.clear();
		Packages.clear();
	}
}
