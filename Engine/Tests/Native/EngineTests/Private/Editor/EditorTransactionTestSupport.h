#pragma once

#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/Transactor.h"

namespace Durin::Tests
{
	// Gives a native test one uniquely named, strongly retained transaction buffer.
	class FTestTransactorOwner final
	{
	public:
		explicit FTestTransactorOwner(std::string_view NamePrefix = "TestTransactor")
			: Transactor(NewObject<DTransBuffer>(nullptr, MakeName(NamePrefix)))
			, TransactorRoot(Transactor)
		{}

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
