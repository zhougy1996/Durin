#pragma once

#include "Editor/Transaction.h"
#include "Math/Transform.h"
#include "Components/SceneComponent.h"
#include "DObject/ObjectPtr.h"

namespace Durin
{
	class AActor;
}

namespace Durin::Editor::Level
{
	// Restores actor parentage and world transforms for one hierarchy edit.
	class FActorAttachmentTransaction final : public ::Durin::Editor::ITransaction
	{
	public:
		struct FEntry
		{
			TObjectPtr<AActor> Actor;
			TObjectPtr<AActor> BeforeParent;
			TObjectPtr<AActor> AfterParent;
			FTransform BeforeTransform;
			FTransform AfterTransform;
		};

		FActorAttachmentTransaction(std::vector<FEntry> InEntries, bool bInAttaching);

		auto GetDescription() const -> std::string_view override;
		auto GetDetails(::Durin::Editor::ETransactionOperation Operation) const -> std::string override;
		auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
		auto Undo() -> bool override { return Apply(false); }
		auto Redo() -> bool override { return Apply(true); }

	private:
		auto Apply(bool bAfter) -> bool;

		std::vector<FEntry> Entries;
		std::vector<DPackage*> AffectedPackages;
		bool bAttaching = false;
	};

	auto MakeActorAttachmentEntry(AActor& Actor, AActor& Parent,
		EAttachmentTransformRule Rule) -> FActorAttachmentTransaction::FEntry;
	auto MakeActorDetachmentEntry(AActor& Actor,
		EDetachmentTransformRule Rule) -> FActorAttachmentTransaction::FEntry;
}
