#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"

#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
namespace Durin::DerivedData { class ICache; }
namespace Durin::AssetPrivate
{
	// Call only after all mesh owner requests retire. Null restores production.
	ENGINE_API auto SetStaticMeshAsyncCacheForTests(std::shared_ptr<DerivedData::ICache> Cache) -> void;
	// Successful DDC completion has reached the mailbox, but its callback is still alive.
	ENGINE_API auto SetStaticMeshBuildCompletionHookForTests(std::function<void()> Hook) -> void;
}
#endif
