#include "Panels/ActorAttachmentTransaction.h"

#include "Engine/Actor.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto CanApplyEntry(const FActorAttachmentTransaction::FEntry& Entry, bool bAfter) -> bool
		{
			AActor* Actor = Entry.Actor.Get();
			AActor* ExpectedParent = bAfter ? Entry.BeforeParent.Get() : Entry.AfterParent.Get();
			AActor* DesiredParent = bAfter ? Entry.AfterParent.Get() : Entry.BeforeParent.Get();
			if (!Actor || !Actor->GetRootComponent() || Actor->GetAttachParentActor() != ExpectedParent)
				return false;
			if (!DesiredParent) return true;
			if (DesiredParent == Actor || !DesiredParent->GetRootComponent()
				|| DesiredParent->GetOuter() != Actor->GetOuter())
				return false;
			for (AActor* Parent = DesiredParent; Parent; Parent = Parent->GetAttachParentActor())
				if (Parent == Actor) return false;
			return true;
		}

		auto SetParentAndTransform(AActor& Actor, AActor* Parent, const FTransform& Transform) -> bool
		{
			const bool bParentChanged = Parent
				? Actor.AttachToActor(Parent, EAttachmentTransformRule::KeepWorld)
				: (!Actor.GetAttachParentActor() || Actor.DetachFromActor(EDetachmentTransformRule::KeepWorld));
			return bParentChanged && Actor.SetActorTransform(Transform);
		}
	}

	FActorAttachmentTransaction::FActorAttachmentTransaction(std::vector<FEntry> InEntries, bool bInAttaching)
		: Entries(std::move(InEntries)), bAttaching(bInAttaching)
	{
		for (const FEntry& Entry : Entries)
		{
			DPackage* Package = Entry.Actor ? Entry.Actor->GetPackage() : nullptr;
			if (Package && std::ranges::find(AffectedPackages, Package) == AffectedPackages.end())
				AffectedPackages.push_back(Package);
		}
	}

	auto FActorAttachmentTransaction::GetDescription() const -> std::string_view
	{
		return bAttaching ? "Attach actors" : "Detach actors";
	}

	auto FActorAttachmentTransaction::GetDetails(::Durin::Editor::ETransactionOperation Operation) const -> std::string
	{
		if (Operation == ::Durin::Editor::ETransactionOperation::Undo)
			return std::format("Restore hierarchy for {} actor(s)", Entries.size());
		return std::format("{} {} actor(s)", bAttaching ? "Attach" : "Detach", Entries.size());
	}

	auto FActorAttachmentTransaction::Apply(bool bAfter) -> bool
	{
		if (Entries.empty() || !std::ranges::all_of(Entries, [bAfter](const FEntry& Entry) {
			return CanApplyEntry(Entry, bAfter);
		})) return false;

		size_t AppliedCount = 0;
		for (const FEntry& Entry : Entries)
		{
			AActor* Actor = Entry.Actor.Get();
			AActor* Parent = bAfter ? Entry.AfterParent.Get() : Entry.BeforeParent.Get();
			const FTransform& Transform = bAfter ? Entry.AfterTransform : Entry.BeforeTransform;
			if (SetParentAndTransform(*Actor, Parent, Transform))
			{
				++AppliedCount;
				continue;
			}

			while (AppliedCount > 0)
			{
				const FEntry& Applied = Entries[--AppliedCount];
				if (AActor* AppliedActor = Applied.Actor.Get())
				{
					AActor* PreviousParent = bAfter ? Applied.BeforeParent.Get() : Applied.AfterParent.Get();
					const FTransform& PreviousTransform = bAfter ? Applied.BeforeTransform : Applied.AfterTransform;
					SetParentAndTransform(*AppliedActor, PreviousParent, PreviousTransform);
				}
			}
			return false;
		}
		return true;
	}
}
