#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DSceneComponent;
	class DClass;
	class DTransactor;
}

namespace Durin::Editor::Level
{
	// Reports a structural component edit, including a rejected transaction or preparation.
	struct FComponentMutationResult
	{
		DActorComponent* Component = nullptr;
		std::string Message;
		explicit operator bool() const { return Component != nullptr && Message.empty(); }
	};

	// Editor-only component structure operations. History retains identity across absence,
	// while preparation completes copying and derived-state rebuilding before registration.
	struct FComponentMutations
	{
		static auto Add(AActor& Owner, DClass* Class, DSceneComponent* Parent,
			DTransactor* Transactions, bool bReadOnly = false) -> FComponentMutationResult;
		static auto Duplicate(AActor& Owner, DActorComponent* Source,
			DTransactor* Transactions, bool bReadOnly = false) -> FComponentMutationResult;
		static auto Remove(AActor& Owner, DActorComponent* Component,
			DTransactor* Transactions, bool bReadOnly = false) -> FComponentMutationResult;
		static auto Rename(AActor& Owner, DActorComponent* Component, FName RequestedName,
			DTransactor* Transactions, bool bReadOnly = false) -> FComponentMutationResult;
		// Keeps the current world transform on execution; replay restores authored relatives.
		static auto Reparent(AActor& Owner, DSceneComponent* Component, DSceneComponent* Parent,
			DTransactor* Transactions, bool bReadOnly = false) -> FComponentMutationResult;
	};
}
