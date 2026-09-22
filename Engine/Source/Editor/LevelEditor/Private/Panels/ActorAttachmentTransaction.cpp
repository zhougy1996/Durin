#include "Panels/ActorAttachmentTransaction.h"

#include "Engine/Actor.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto CanApplyEntry(const FActorAttachmentTransaction::FEntry& Entry, bool bAfter) -> FTransactionCustomResult
		{
			AActor* Actor = Entry.Actor.Get();
			AActor* ExpectedParent = bAfter ? Entry.BeforeParent.Get() : Entry.AfterParent.Get();
			AActor* DesiredParent = bAfter ? Entry.AfterParent.Get() : Entry.BeforeParent.Get();
			if (!Actor || !Actor->GetRootComponent()) return {{.Code = ETransactionCustomError::TargetUnavailable}};
			if (Actor->GetAttachParentActor() != ExpectedParent)
				return {{.Code = ETransactionCustomError::ParentMismatch, .TargetPath = Actor->GetObjectPath(),
					.ExpectedParentPath = ExpectedParent ? ExpectedParent->GetObjectPath() : std::string{},
					.ActualParentPath = Actor->GetAttachParentActor() ? Actor->GetAttachParentActor()->GetObjectPath() : std::string{}}};
			if (!DesiredParent) return {};
			if (DesiredParent == Actor || !DesiredParent->GetRootComponent()
				|| DesiredParent->GetOuter() != Actor->GetOuter())
				return {{.Code = ETransactionCustomError::ParentInvalid, .TargetPath = Actor->GetObjectPath(),
					.ExpectedParentPath = DesiredParent->GetObjectPath()}};
			for (AActor* Parent = DesiredParent; Parent; Parent = Parent->GetAttachParentActor())
				if (Parent == Actor) return {{.Code = ETransactionCustomError::ParentCycle, .TargetPath = Actor->GetObjectPath(),
					.ExpectedParentPath = DesiredParent->GetObjectPath()}};
			return {};
		}

		auto SetParentAndTransform(AActor& Actor, AActor* Parent, const FTransform& Transform) -> bool
		{
			const bool bParentChanged = Parent
				? Actor.AttachToActor(Parent, EAttachmentTransformRule::KeepWorld)
				: (!Actor.GetAttachParentActor() || Actor.DetachFromActor(EDetachmentTransformRule::KeepWorld));
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

	auto FActorAttachmentTransaction::GetDetails(::Durin::Editor::ETransactionOperation Operation) const -> std::string
	{
		if (Operation == ::Durin::Editor::ETransactionOperation::Undo)
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

	auto FActorAttachmentTransaction::Apply(bool bAfter) -> FTransactionCustomResult
	{
		if (Entries.empty()) return {{.Code = ETransactionCustomError::EmptySelection}};
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
			return {{.Code = ETransactionCustomError::AttachmentWrite, .TargetPath = Actor->GetObjectPath(),
				.ExpectedParentPath = Parent ? Parent->GetObjectPath() : std::string{}, .NodeCount = Entries.size(),
				.MemberIndex = static_cast<size_t>(&Entry - Entries.data())}};
		}
		return {};
	}
}
