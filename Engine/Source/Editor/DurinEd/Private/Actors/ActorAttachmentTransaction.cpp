#include "Actors/ActorAttachmentTransaction.h"
#include "Actors/ActorChange.h"
#include "Actors/ActorEditingValidation.h"

#include "Engine/Actor.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin::Editor
{
	auto FActorAttachmentTransaction::GetModifiedObjects() const -> std::vector<DObject*>
	{
		std::vector<DObject*> Objects;
		for (const auto& Entry : Entries)
		{
			Objects.push_back(Entry.Actor.Get());
			for (auto* Actor : {Entry.Actor.Get(), Entry.BeforeParent.Get(), Entry.AfterParent.Get()})
				if (Actor && Actor->GetRootComponent()) Objects.push_back(Actor->GetRootComponent());
		}
		return Objects;
	}

	namespace
	{
		auto CanApplyEntry(const FActorAttachmentTransaction::FEntry& Entry, bool bAfter) -> FActorChangeResult
		{
			AActor* Actor = Entry.Actor.Get();
			AActor* ExpectedParent = bAfter ? Entry.BeforeParent.Get() : Entry.AfterParent.Get();
			AActor* DesiredParent = bAfter ? Entry.AfterParent.Get() : Entry.BeforeParent.Get();
			auto* Level = IsValid(Actor) ? Cast<DLevel>(Actor->GetOuter()) : nullptr;
			if (!CanEditActorOwner(Actor) || !Actor->GetRootComponent())
				return {{.Code = EActorChangeError::TargetUnavailable}};
			if (Actor->GetAttachParentActor() != ExpectedParent
				&& (Actor->GetAttachParentActor() != DesiredParent || !Actor->GetRootComponent()->IsAttachmentChangedDuringUndo()))
				return {{.Code = EActorChangeError::ParentMismatch, .TargetPath = Actor->GetObjectPath(),
					.ExpectedParentPath = ExpectedParent ? ExpectedParent->GetObjectPath() : std::string{},
					.ActualParentPath = Actor->GetAttachParentActor() ? Actor->GetAttachParentActor()->GetObjectPath() : std::string{}}};
			if (!DesiredParent) return {};
			if (!IsValid(DesiredParent) || !Level->ContainsActor(DesiredParent) || DesiredParent->IsBeingDestroyed()
				|| DesiredParent->HasBegunPlay() || DesiredParent == Actor || !DesiredParent->GetRootComponent()
				|| DesiredParent->GetOuter() != Actor->GetOuter())
				return {{.Code = EActorChangeError::ParentInvalid, .TargetPath = Actor->GetObjectPath(),
					.ExpectedParentPath = DesiredParent->GetObjectPath()}};
			for (AActor* Parent = DesiredParent; Parent; Parent = Parent->GetAttachParentActor())
				if (Parent == Actor) return {{.Code = EActorChangeError::ParentCycle, .TargetPath = Actor->GetObjectPath(),
					.ExpectedParentPath = DesiredParent->GetObjectPath()}};
			return {};
		}

		auto SetParentAndTransform(AActor& Actor, AActor* Parent, const FTransform& Transform) -> bool
		{
			const bool bParentChanged = Actor.GetAttachParentActor() == Parent || (Parent
				? Actor.AttachToActor(Parent, EAttachmentTransformRule::KeepWorld)
				: Actor.DetachFromActor(EDetachmentTransformRule::KeepWorld));
			return bParentChanged && Actor.SetActorTransform(Transform);
		}
	}

	auto MakeActorAttachmentEntry(AActor& Actor, AActor& Parent,
		EAttachmentTransformRule Rule) -> FActorAttachmentTransaction::FEntry
	{
		const FTransform BeforeTransform = Actor.GetActorTransform();
		FTransform AfterTransform = BeforeTransform;
		switch (Rule)
		{
		case EAttachmentTransformRule::KeepWorld:
			break;
		case EAttachmentTransformRule::KeepRelative:
			if (DSceneComponent* RootComponent = Actor.GetRootComponent())
				AfterTransform = FTransform::Combine(
					Parent.GetActorTransform(), RootComponent->GetRelativeTransform());
			break;
		case EAttachmentTransformRule::SnapToTarget:
			AfterTransform = Parent.GetActorTransform();
			break;
		}
		return {&Actor, Actor.GetAttachParentActor(), &Parent, BeforeTransform, AfterTransform};
	}

	auto MakeActorDetachmentEntry(AActor& Actor,
		EDetachmentTransformRule Rule) -> FActorAttachmentTransaction::FEntry
	{
		const FTransform BeforeTransform = Actor.GetActorTransform();
		const DSceneComponent* RootComponent = Actor.GetRootComponent();
		const FTransform AfterTransform = Rule == EDetachmentTransformRule::KeepRelative && RootComponent
			? RootComponent->GetRelativeTransform() : BeforeTransform;
		return {&Actor, Actor.GetAttachParentActor(), nullptr, BeforeTransform, AfterTransform};
	}

	FActorAttachmentTransaction::FActorAttachmentTransaction(std::vector<FEntry> InEntries, bool bInAttaching)
		: Entries(std::move(InEntries)), bAttaching(bInAttaching)
	{
		for (const FEntry& Entry : Entries)
		{
			DPackage* Package = Entry.Actor ? Entry.Actor->GetPackage() : nullptr;
			if (Package && !std::ranges::contains(AffectedPackages, Package))
				AffectedPackages.push_back(Package);
		}
	}

	auto FActorAttachmentTransaction::GetDescription() const -> std::string_view
	{
		return bAttaching ? "Attach actors" : "Detach actors";
	}

	auto FActorAttachmentTransaction::GetDetails(::Durin::ETransactionOperation Operation) const -> std::string
	{
		if (Operation == ::Durin::ETransactionOperation::Undo)
			return std::format("Restore hierarchy for {} actor(s)", Entries.size());
		return std::format("{} {} actor(s)", bAttaching ? "Attach" : "Detach", Entries.size());
	}

	auto FActorAttachmentTransaction::AddReferencedObjects(
		FReferenceCollector& Collector) const -> void
	{
		for (const FEntry& Entry : Entries)
			for (DObject* Object : {static_cast<DObject*>(Entry.Actor.Get()),
				static_cast<DObject*>(Entry.BeforeParent.Get()),
				static_cast<DObject*>(Entry.AfterParent.Get())})
				if (Object) Collector.AddReferencedObject(Object);
	}

	auto FActorAttachmentTransaction::Apply(bool bAfter) -> FActorChangeResult
	{
		FScopedPackageDirtySuppression SuppressReplay;
		if (Entries.empty()) return {{.Code = EActorChangeError::EmptySelection}};
		for (size_t Index = 0; Index < Entries.size(); ++Index)
			if (auto Validated = CanApplyEntry(Entries[Index], bAfter); !Validated)
			{
				Validated.Error.MemberIndex = Index;
				Validated.Error.NodeCount = Entries.size();
				return Validated;
			}

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

			// The rejecting write may already have changed parentage before transform
			// restoration failed. Compensate it as well as successful earlier entries.
			bool bRestored = SetParentAndTransform(*Actor, (bAfter ? Entry.BeforeParent : Entry.AfterParent).Get(),
				bAfter ? Entry.BeforeTransform : Entry.AfterTransform);
			while (AppliedCount > 0)
			{
				const FEntry& Applied = Entries[--AppliedCount];
				if (AActor* AppliedActor = Applied.Actor.Get())
				{
					AActor* PreviousParent = bAfter ? Applied.BeforeParent.Get() : Applied.AfterParent.Get();
					const FTransform& PreviousTransform = bAfter ? Applied.BeforeTransform : Applied.AfterTransform;
					bRestored = SetParentAndTransform(*AppliedActor, PreviousParent, PreviousTransform) && bRestored;
				}
				else bRestored = false;
			}
			return {{.Code = EActorChangeError::AttachmentWrite, .TargetPath = Actor->GetObjectPath(),
				.ExpectedParentPath = Parent ? Parent->GetObjectPath() : std::string{}, .NodeCount = Entries.size(),
				.MemberIndex = static_cast<size_t>(&Entry - Entries.data()),
				.CleanupCause = bRestored ? nullptr : std::make_shared<FActorChangeError>(FActorChangeError{.Code = EActorChangeError::RollbackIncomplete})}};
		}
		return {};
	}
}
