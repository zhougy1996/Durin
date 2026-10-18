#pragma once

#include "CoreMinimal.h"
#include "DerivedDataCacheAPI.h"

namespace Durin::DerivedData
{
	struct FCacheDiagnosticCounters
	{
		uint64 Gets = 0, Hits = 0, Misses = 0, InvalidRecords = 0, BackendFailures = 0, Canceled = 0;
		uint64 RequestRejections = 0;
		uint64 Puts = 0, StoreFailures = 0, CapacityRejections = 0, BuildFallbacks = 0, SchemaRejections = 0;
		uint64 ReadBytes = 0, DecodedBytes = 0, WrittenBytes = 0, EncodedRawBytes = 0;
		uint64 BlobEncodes = 0, BlobsReused = 0, GetNanoseconds = 0, PutNanoseconds = 0;
		uint64 AsyncStarts = 0, QueueNanoseconds = 0;
	};
	struct FCacheBucketDiagnostics { std::string Bucket; FCacheDiagnosticCounters Counters; };
	struct FCacheDiagnostics
	{
		static constexpr uint32 MaximumBuckets = 64;
		FCacheDiagnosticCounters Total, OtherBuckets;
		std::vector<FCacheBucketDiagnostics> Buckets;
	};
	// Process-local cumulative snapshots for the local backend. No keys, payloads,
	// per-request history, persisted metrics, resets, or background scanning.
	DERIVEDDATACACHE_API auto GetCacheDiagnostics() -> FCacheDiagnostics;
	// Hosts call after draining asset/cache work and before logger shutdown.
	DERIVEDDATACACHE_API auto LogCacheDiagnostics() -> void;
}
