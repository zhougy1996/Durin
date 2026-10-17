#pragma once

#include "CoreMinimal.h"
#include "Actors/EditorActorSubsystem.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin
{
	struct FEditorActorSubsystemTestAccess
	{
		static auto Inject(DEditorActorSubsystem& Service, DTransactor* Transactions) -> void { Service.TestTransactions = Transactions; }
	};
}
namespace Durin::Tests
{
	// Independent service instances exercise production operations with one test history.
	class FTestActorEditor
	{
	public:
		explicit FTestActorEditor(DTransactor* Transactions) : Service(NewObject<DEditorActorSubsystem>(nullptr, {}))
		{ FEditorActorSubsystemTestAccess::Inject(*Service, Transactions); }
		auto operator->() const -> DEditorActorSubsystem* { return Service.Get(); }
	private:
		TStrongObjectPtr<DEditorActorSubsystem> Service;
	};
}
