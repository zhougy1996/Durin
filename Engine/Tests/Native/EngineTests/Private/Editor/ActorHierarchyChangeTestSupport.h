#pragma once

#include "CoreMinimal.h"
#include "EditorSupportDelegates.h"

#if DURIN_WITH_EDITOR
namespace Durin::Tests
{
	class FActorHierarchyChangeObserver
	{
	public:
		explicit FActorHierarchyChangeObserver(DLevel& Level)
			: Handle(FEditorSupportDelegates::ActorHierarchyChanged.AddLambda([this, &Level](DLevel& Changed) {
				if (&Changed == &Level) bChanged = true;
			})) {}
		~FActorHierarchyChangeObserver() { FEditorSupportDelegates::ActorHierarchyChanged.Remove(Handle); }
		FActorHierarchyChangeObserver(const FActorHierarchyChangeObserver&) = delete;
		auto operator=(const FActorHierarchyChangeObserver&) -> FActorHierarchyChangeObserver& = delete;
		auto Consume() -> bool { return std::exchange(bChanged, false); }
	private:
		FDelegateHandle Handle;
		bool bChanged = false;
	};
}
#endif
