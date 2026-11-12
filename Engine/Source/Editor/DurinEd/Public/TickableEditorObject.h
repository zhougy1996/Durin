#pragma once
#include "CoreMinimal.h"
#include "Tickable.h"
#include "DurinEdAPI.h"
namespace Durin
{
	class DEditorEngine;
	// Owner-thread editor callbacks, independent of worlds, views and game ticking.
	class FTickableEditorObject : public FTickableObjectBase
	{
	public:
		DURINED_API explicit FTickableEditorObject();
		DURINED_API ~FTickableEditorObject() override;
		FTickableEditorObject(const FTickableEditorObject&) = delete;
		auto operator=(const FTickableEditorObject&) -> FTickableEditorObject& = delete;
		virtual auto GetTickableEditor() const -> DEditorEngine* = 0;
		DURINED_API static auto TickObjects(DEditorEngine&, float DeltaSeconds) -> void;
	};
}
