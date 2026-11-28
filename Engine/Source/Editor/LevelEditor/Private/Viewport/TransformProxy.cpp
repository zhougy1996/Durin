#include "Viewport/TransformProxy.h"

#include "Actors/ActorChange.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Math/Operations.h"
#include "Math/TransformDecomposition.h"
#include "Transactions/Transactor.h"

namespace Durin::LevelEditor
{
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
		// Restores before/after transforms for the changed elements of one data owner.
		class FTransformProxyChange final : public FCommandChange
		{
		public:
			struct FEntry
			{
				std::shared_ptr<ITransformGizmoTarget> Target;
				FTransform Before;
				FTransform After;
			};
			FTransformProxyChange(std::string_view Action, std::string_view CollectionLabel, std::vector<FEntry> InEntries)
				: Entries(std::move(InEntries))
			{
				for (const FEntry& Entry : Entries)
				{
					DPackage* Package = Entry.Target ? Entry.Target->GetPackage() : nullptr;
					if (Package && !std::ranges::contains(AffectedPackages, Package))
						AffectedPackages.push_back(Package);
				}
				if (Entries.size() == 1 && Entries.front().Target)
					Description = std::format("{} '{}'", Action, Entries.front().Target->GetLabel());
				else
					Description = std::format("{} {} {}", Action, Entries.size(), CollectionLabel);
			}
			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }

			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Apply(DObject* Owner) -> void override { SetTransforms(Owner, true); }
			auto Revert(DObject* Owner) -> void override { SetTransforms(Owner, false); }
			auto AddReferencedObjects(FReferenceCollector& Collector) -> void override
			{
				for (const FEntry& Entry : Entries)
					if (Entry.Target) Entry.Target->AddReferencedObjects(Collector);
				for (auto& Package : AffectedPackages)
				{
					DObject* Object = Package;
					Collector.AddReferencedObject(Object);
					if (Package != Object) Package = Cast<DPackage>(Object);
				}
			}

		private:
			static auto FormatVector(const FVector3& Value) -> std::string
			{
				return std::format("({:.3f}, {:.3f}, {:.3f})", Value.x, Value.y, Value.z);
			}

			static auto VectorChanged(const FVector3& Before, const FVector3& After) -> bool
			{
				return Math::Length(After - Before) > Epsilon;
			}

			static auto RotationChanged(const FQuat& Before, const FQuat& After) -> bool
			{
				return 1.0 - std::abs(Math::Dot(Math::Normalize(Before), Math::Normalize(After))) > Epsilon;
			}

			auto GetDetails() const -> std::string override
			{
				std::string Result;
				for (const FEntry& Entry : Entries)
				{
					if (!Result.empty()) Result += '\n';
					Result += std::format("Recorded edit to '{}'", Entry.Target ? Entry.Target->GetLabel() : "Missing Target");
					const FTransform& Before = Entry.Before;
					const FTransform& After = Entry.After;
					bool bHasChange = false;
					if (VectorChanged(Before.Translation, After.Translation))
					{
						Result += std::format("\n  Location  {} -> {}", FormatVector(Before.Translation), FormatVector(After.Translation));
						Result += std::format("\n  Delta     {}", FormatVector(After.Translation - Before.Translation));
						bHasChange = true;
					}
					if (RotationChanged(Before.Rotation, After.Rotation))
					{
						const FVector3 BeforeDegrees = Math::QuaternionToEulerDegrees(Before.Rotation);
						const FVector3 AfterDegrees = Math::QuaternionToEulerDegrees(After.Rotation);
						Result += std::format("\n  Rotation  {} -> {} degrees", FormatVector(BeforeDegrees), FormatVector(AfterDegrees));
						bHasChange = true;
					}
					if (VectorChanged(Before.Scale3D, After.Scale3D))
					{
						Result += std::format("\n  Scale     {} -> {}", FormatVector(Before.Scale3D), FormatVector(After.Scale3D));
						bHasChange = true;
					}
					if (!bHasChange) Result += "\n  Transform changed below display precision";
				}
				return Result;
			}

			auto HasExpired(const DObject* Owner) const noexcept -> bool override
			{
				return !Owner || std::ranges::any_of(Entries, [&](const auto& Entry) {
					return !Entry.Target || Entry.Target->GetDataOwner() != Owner || Entry.Target->IsHistoryInvalidated() || !Entry.Target->IsValid();
				});
			}

			auto SetTransforms(DObject* Owner, bool bAfter) -> void
			{
				for (size_t Index = 0; Index < Entries.size(); ++Index)
				{
					const auto& Target = Entries[Index].Target;
					if (!Target || Target->GetDataOwner() != Owner || Target->IsHistoryInvalidated() || !Target->IsValid())
						throw std::runtime_error(FormatActorChangeError({.Code = EActorChangeError::TargetUnavailable, .MemberIndex = Index}));
				}
				size_t FailedIndex = Entries.size();
				for (size_t Index = 0; Index < Entries.size(); ++Index)
					if (!Entries[Index].Target->SetTransform(bAfter ? Entries[Index].After : Entries[Index].Before))
					{ FailedIndex = Index; break; }
				if (FailedIndex == Entries.size())
					for (size_t Index = 0; Index < Entries.size(); ++Index)
						if (!Entries[Index].Target->IsValid() || !SameTransform(Entries[Index].Target->GetTransform(), bAfter ? Entries[Index].After : Entries[Index].Before))
						{ FailedIndex = Index; break; }
				if (FailedIndex != Entries.size())
					throw std::runtime_error(FormatActorChangeError({.Code = EActorChangeError::TransformWrite,
						.TargetLabel = Entries[FailedIndex].Target->GetLabel(), .NodeCount = Entries.size(), .MemberIndex = FailedIndex}));
			}

			std::string Description;
			std::vector<FEntry> Entries;
			std::vector<DPackage*> AffectedPackages;
		};
	}

	FTransformProxy::FTransformProxy(const FTransformGizmoTargetSet& Targets, const FVector3& Pivot, const FQuat& Basis)
		: CollectionLabel(Targets.CollectionLabel)
	{
		InitialFrame.Translation = Pivot;
		InitialFrame.Rotation = Basis;
		for (const std::shared_ptr<ITransformGizmoTarget>& Target : Targets.Targets)
		{
			if (Target && Target->IsValid())
			{
				const auto Initial = Target->GetTransform();
				Snapshots.push_back({Target, Target->GetIdentity(), FObjectKey(Target->GetDataOwner()), Initial, Initial, Target->GetPackage()});
				if (DPackage* Package = Target->GetPackage(); Package && !std::ranges::contains(Packages, Package, &TObjectPtr<DPackage>::Get))
					Packages.emplace_back(Package);
			}
		}
	}

	auto FTransformProxy::SetTransform(const FTransform& Frame) -> bool
	{
		const FVector3 Offset = Frame.Translation - InitialFrame.Translation;
		const bool bRotate = Frame.Rotation != InitialFrame.Rotation;
		const bool bScale = Frame.Scale3D != InitialFrame.Scale3D;
		const FQuat Rotation = Frame.Rotation * Math::Inverse(InitialFrame.Rotation);
		const auto& Pivot = InitialFrame.Translation;
		FMatrix Scale(1.0);
		if (bScale)
			Scale = Math::TranslationMatrix(Pivot) * Math::RotationMatrix(InitialFrame.Rotation)
				* Math::ScaleMatrix(Frame.Scale3D) * Math::RotationMatrix(Math::Inverse(InitialFrame.Rotation))
				* Math::TranslationMatrix(-Pivot);

		for (const auto& Snapshot : Snapshots)
		{
			if (!Snapshot.Target->IsValid()) return false;
			FTransform Transform = Snapshot.Initial;
			if (bScale && !TryMakeTransformFromMatrix(Scale * Snapshot.Initial.ToMatrix(), Transform)) return false;
			if (bRotate)
			{
				Transform.Translation = Pivot + Rotation * (Transform.Translation - Pivot);
				Transform.Rotation = Math::Normalize(Rotation * Transform.Rotation);
			}
			Transform.Translation += Offset;
			if (!TrySetTransform(*Snapshot.Target, Transform)) return false;
		}
		return true;
	}

	auto FTransformProxy::MatchesSelection(const FTransformGizmoTargetSet& Targets) const -> bool
	{
		return !Snapshots.empty() && Targets.Targets.size() == Snapshots.size()
			&& std::ranges::all_of(Snapshots, [&](const auto& Snapshot) {
				return Snapshot.Target->IsValid() && std::ranges::any_of(Targets.Targets, [&](const auto& Target) {
					return Target && Target->GetIdentity() == Snapshot.Identity;
				});
			});
	}

	auto FTransformProxy::HasChanges() const -> bool
	{
		return std::ranges::any_of(Snapshots, [](const auto& Snapshot) {
			return !SameTransform(Snapshot.Initial, Snapshot.Preview);
		});
	}

	auto FTransformProxy::CheckPreviewOwnership() -> bool
	{
		bool bOwned = true;
		for (auto& Snapshot : Snapshots)
		{
			if (!Snapshot.bOwnsPreview || !Snapshot.Target || !Snapshot.Target->IsValid()
				|| FObjectKey(Snapshot.Target->GetDataOwner()) != Snapshot.Owner
				|| !SameTransform(Snapshot.Target->GetTransform(), Snapshot.Preview))
			{
				Snapshot.bOwnsPreview = false;
				if (Snapshot.Package) Snapshot.Package->MarkDirty();
				bOwned = false;
			}
		}
		return bOwned;
	}

	auto FTransformProxy::CapturePreviewValues() -> bool
	{
		bool bValid = true;
		for (auto& Snapshot : Snapshots)
		{
			if (Snapshot.Target && Snapshot.Target->IsValid()) Snapshot.Preview = Snapshot.Target->GetTransform();
			else bValid = false;
		}
		return bValid;
	}

	auto FTransformProxy::GetPackageEditRevisions() const -> std::vector<std::pair<TObjectPtr<DPackage>, uint64>>
	{
		std::vector<std::pair<TObjectPtr<DPackage>, uint64>> Revisions;
		for (const auto& Package : Packages)
			if (Package) Revisions.emplace_back(Package, Package->GetEditRevision());
		return Revisions;
	}

	auto FTransformProxy::Restore() -> bool
	{
		CheckPreviewOwnership();
		bool bRestored = true;
		for (auto& Snapshot : Snapshots)
			if (Snapshot.bOwnsPreview && !TrySetTransform(*Snapshot.Target, Snapshot.Initial)) bRestored = false;
		CapturePreviewValues();
		for (const auto& Snapshot : Snapshots)
			if (Snapshot.bOwnsPreview && (!Snapshot.Target->IsValid() || !SameTransform(Snapshot.Preview, Snapshot.Initial))) bRestored = false;
		if (!bRestored)
		{
			for (const auto& Package : Packages)
				if (Package) Package->MarkDirty();
		}
		return bRestored;
	}


	auto FTransformProxy::RecordChanges(DTransactor& Transactions, std::string_view Action) -> bool try
	{
		struct FOwnerChange
		{
			TStrongObjectPtr<DObject> Owner;
			std::vector<FTransformProxyChange::FEntry> Entries;
		};
		std::vector<FOwnerChange> Groups;
		std::unordered_map<DObject*, size_t> OwnerIndices;
		for (const auto& Snapshot : Snapshots)
		{
			if (SameTransform(Snapshot.Initial, Snapshot.Preview)) continue;
			auto Target = Snapshot.Target->CreateHistoryTarget(Transactions.GetHistoryId());
			if (!Target) Target = Snapshot.Target;
			auto* Owner = Target->GetDataOwner();
			if (!IsValid(Owner) || FObjectKey(Owner) != Snapshot.Owner) return false;
			const auto [It, Added] = OwnerIndices.try_emplace(Owner, Groups.size());
			if (Added) Groups.push_back({TStrongObjectPtr<DObject>(Owner), {}});
			Groups[It->second].Entries.push_back({std::move(Target), Snapshot.Initial, Snapshot.Preview});
		}
		if (Groups.empty()) return false;
		const auto Description = Snapshots.size() == 1 ? std::format("{} '{}'", Action, Snapshots.front().Target->GetLabel())
			: std::format("{} {} {}", Action, Snapshots.size(), CollectionLabel);
		if (GUndo || ResolveEditorTransactor(true).value_or(nullptr) != &Transactions || !CheckPreviewOwnership()) return false;
		FScopedTransaction Recording("TransformGizmo", Description, nullptr);
		if (!Recording.IsOutstanding()) return false;
		try
		{
			if (CheckPreviewOwnership())
			{
				for (auto& Group : Groups)
				{
					auto Change = std::make_unique<FTransformProxyChange>(Action, CollectionLabel, std::move(Group.Entries));
					if (!IsValid(Group.Owner.Get()) || !Transactions.StoreUndo(Group.Owner.Get(), std::move(Change)))
					{
						Recording.Cancel();
						return false;
					}
				}
				return true;
			}
		}
		catch (...)
		{
			Recording.Cancel();
			throw;
		}
		Recording.Cancel();
		return false;
	}
	catch (const std::exception& Error) { DURIN_WARN("Transform recording failed: {}", Error.what()); return false; }
	catch (...) { DURIN_WARN("Transform recording failed."); return false; }
}
