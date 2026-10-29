#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
	#include "CacheDispatchPrivate.h"
#endif
#include "EngineAPI.h"

#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
namespace Durin::DerivedData
{
	class ICacheBackend;
}
namespace Durin::AssetPrivate
{
	// Call only after all mesh owner requests retire. Null restores production.
	ENGINE_API auto SetStaticMeshAsyncCacheForTests(std::shared_ptr<DerivedData::ICacheBackend> Cache) -> void;
	// Successful DDC completion has reached the mailbox, but its callback is still alive.
	ENGINE_API auto SetStaticMeshBuildCompletionHookForTests(std::function<void()> Hook) -> void;
} // namespace Durin::AssetPrivate
#endif
