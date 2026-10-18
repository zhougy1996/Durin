#pragma once

#include "CoreMinimal.h"
#include "DerivedDataCache/DerivedDataCacheDiagnostics.h"
#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::DerivedData::Private
{
	auto ObserveCacheGet(const FCacheKey& Key, const FCacheGetResult& Result, const FCacheQueryStats& Stats) -> void;
	auto ObserveCachePut(const FCacheKey& Key, const FCachePutResult& Result, const FCacheStoreStats& Stats, uint64 Nanoseconds) -> void;
	enum class ECacheObservation { CapacityRejected, Fallback, SchemaRejected, RecordRejected, AsyncStart };
	auto ObserveCache(const FCacheKey& Key, ECacheObservation Event, uint64 Nanoseconds = 0) -> void;
	DERIVEDDATACACHE_API auto ResetCacheDiagnosticsForTests() -> void;
}
