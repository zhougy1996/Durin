#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectPtr.h"

namespace Durin { class AActor; class DClass; class DLevel; class DTransactor; }
namespace Durin::Editor { class ITransactionCustomChange; }

namespace Durin::Editor::Level
{
	struct FActorMutationResult
	{
		std::vector<TObjectPtr<AActor>> Actors;
		std::string Message;
		explicit operator bool() const { return Message.empty(); }
	};

	// All interactive actor structure commands use exact identities and one history order.
	struct FActorMutations
	{
		static auto Add(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
			DTransactor* Transactions, bool bReadOnly = false,
			std::function<void(AActor&)> Configure = {}) -> FActorMutationResult;
		static auto Remove(DLevel& Level, std::span<AActor* const> Actors,
			DTransactor* Transactions, bool bReadOnly = false) -> FActorMutationResult;
		static auto Duplicate(DLevel& Level, std::span<AActor* const> Actors,
			DTransactor* Transactions, bool bReadOnly = false) -> FActorMutationResult;
		static auto Rename(DLevel& Level, AActor* Actor, FName Name,
			DTransactor* Transactions, bool bReadOnly = false) -> FActorMutationResult;
		// Prepared actors must be live, constructed, and not yet published in the Level.
		static auto MakePresenceChange(DLevel& Level, std::span<AActor* const> Actors,
			bool bRemove, std::string Description) -> std::unique_ptr<ITransactionCustomChange>;
	};
}
