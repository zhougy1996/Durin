#pragma once

#include "CoreMinimal.h"

#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "Transactions/Transactor.h"

namespace Durin
{
	struct FTransBufferTestAccess
	{
		static auto InitializeMemoryBudget(DTransBuffer& Buffer, size_t Bytes) -> void
		{
			check(Bytes != 0 && Buffer.NextTransactionId == 1 && !Buffer.Pending && Buffer.History.empty());
			Buffer.MemoryBudget = Bytes;
		}
		static auto InstallHistory(DTransBuffer& Buffer, Editor::FTransaction Transaction) -> void
		{
			Buffer.History.push_back(std::move(Transaction));
			Buffer.Cursor = 1;
		}
		static auto SetState(DTransBuffer& Buffer, Editor::ETransactorState State) -> void
		{
			Buffer.State = State;
		}
	};
}

namespace Durin::Tests
{
	// Gives a native test one uniquely named, strongly retained transaction buffer.
	class FTestTransactorOwner final
	{
	public:
		explicit FTestTransactorOwner(std::string_view NamePrefix = "TestTransactor", size_t MemoryBudget = 64u * 1024u * 1024u)
			: Transactor(NewObject<DTransBuffer>(nullptr, MakeName(NamePrefix)))
			, TransactorRoot(Transactor)
		{ FTransBufferTestAccess::InitializeMemoryBudget(*Transactor, MemoryBudget); }

		auto Get() const -> DTransBuffer* { return Transactor; }
		auto operator->() const -> DTransBuffer* { return Transactor; }

	private:
		static auto MakeName(std::string_view Prefix) -> FName
		{
			static uint64 NextId = 1;
			return FName(std::string(Prefix) + std::to_string(NextId++));
		}

		DTransBuffer* Transactor = nullptr;
		TStrongObjectPtr<DObject> TransactorRoot;
	};
}
