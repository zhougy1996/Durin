#include "CacheDiagnosticsPrivate.h"
#include "Logging/LogMacros.h"

namespace Durin::DerivedData
{
	namespace
	{
		std::mutex DiagnosticMutex;
		FCacheDiagnostics Diagnostics;
		template<typename F> auto Update(const FCacheKey& Key, F&& Apply) noexcept -> void
		try
		{
			std::lock_guard Lock(DiagnosticMutex);
			Apply(Diagnostics.Total);
			const auto Name = Key.GetBucket().ToString();
			auto It = std::ranges::find(Diagnostics.Buckets, Name, &FCacheBucketDiagnostics::Bucket);
			if (It == Diagnostics.Buckets.end() && Diagnostics.Buckets.size() < FCacheDiagnostics::MaximumBuckets)
			{
				Diagnostics.Buckets.push_back({std::string(Name), {}}); It = std::prev(Diagnostics.Buckets.end());
			}
			Apply(It == Diagnostics.Buckets.end() ? Diagnostics.OtherBuckets : It->Counters);
		}
		catch (...) {}
		auto CountError(FCacheDiagnosticCounters& Counters, ECacheError Error) -> void
		{
			if (Error == ECacheError::Canceled) ++Counters.Canceled;
			else if (Error == ECacheError::StorageFailure) ++Counters.BackendFailures;
			else if (Error == ECacheError::Corrupt || Error == ECacheError::MissingValue) ++Counters.InvalidRecords;
			else if (Error == ECacheError::Capacity) ++Counters.CapacityRejections;
			else ++Counters.RequestRejections;
		}
	}
	auto GetCacheDiagnostics() -> FCacheDiagnostics
	{
		std::lock_guard Lock(DiagnosticMutex);
		auto Result = Diagnostics; std::ranges::sort(Result.Buckets, {}, &FCacheBucketDiagnostics::Bucket); return Result;
	}
	auto LogCacheDiagnostics() -> void
	try
	{
		const auto Snapshot = DerivedData::GetCacheDiagnostics();
		const auto HasActivity = [](const DerivedData::FCacheDiagnosticCounters& C) {
			return C != FCacheDiagnosticCounters{};
		};
		if (!HasActivity(Snapshot.Total)) return;

		const auto& Total = Snapshot.Total;
		std::string Events;
		const auto AppendEvent = [&Events](std::string_view Name, uint64 Count) {
			if (Count) Events += std::format(" | {}={}", Name, Count);
		};
		AppendEvent("invalid", Total.InvalidRecords);
		AppendEvent("schema_rejected", Total.SchemaRejections);
		AppendEvent("backend_failed", Total.BackendFailures);
		AppendEvent("request_rejected", Total.RequestRejections);
		AppendEvent("store_failed", Total.StoreFailures);
		AppendEvent("capacity_rejected", Total.CapacityRejections);
		AppendEvent("fallback", Total.BuildFallbacks);
		AppendEvent("canceled", Total.Canceled);
		constexpr double BytesPerMiB = 1024.0 * 1024.0;
		DURIN_INFO_CATEGORY("DDCStats", "Cache summary: {}/{} hits ({:.1f}%) | {} put requests | read {:.2f} MiB, wrote {:.2f} MiB{}",
			Total.Hits, Total.Gets, Total.Gets ? 100.0 * Total.Hits / Total.Gets : 0.0,
			Total.Puts, Total.ReadBytes / BytesPerMiB, Total.WrittenBytes / BytesPerMiB, Events);

		const auto Log = [](std::string_view Bucket, const DerivedData::FCacheDiagnosticCounters& C) {
			DURIN_TRACE_CATEGORY("DDCStats", "bucket={} gets={} hits={} misses={} invalid={} schema_rejected={} backend_failed={} request_rejected={} puts={} store_failed={} capacity_rejected={} fallback={} canceled={} read_bytes={} decoded_bytes={} written_bytes={} encoded_raw_bytes={} blob_encodes={} reused={} get_ns={} put_ns={} async_starts={} queue_ns={}",
				Bucket, C.Gets, C.Hits, C.Misses, C.InvalidRecords, C.SchemaRejections, C.BackendFailures, C.RequestRejections, C.Puts, C.StoreFailures, C.CapacityRejections, C.BuildFallbacks, C.Canceled, C.ReadBytes, C.DecodedBytes, C.WrittenBytes, C.EncodedRawBytes, C.BlobEncodes, C.BlobsReused, C.GetNanoseconds, C.PutNanoseconds, C.AsyncStarts, C.QueueNanoseconds);
		};
		Log("total", Snapshot.Total);
		for (const auto& Bucket : Snapshot.Buckets)
		{
			if (HasActivity(Bucket.Counters)) Log(Bucket.Bucket, Bucket.Counters);
		}
		if (HasActivity(Snapshot.OtherBuckets)) Log("other", Snapshot.OtherBuckets);
	}

	catch (...) {} // Best-effort telemetry must not change host shutdown.

	namespace Private
	{
		auto ResetCacheDiagnosticsForTests() -> void { std::lock_guard Lock(DiagnosticMutex); Diagnostics = {}; }
		auto ObserveCacheGet(const FCacheKey& Key, const FCacheGetResult& Result, const FCacheQueryStats& Stats) -> void
		{
			Update(Key, [&](auto& C) {
				++C.Gets; C.ReadBytes += Stats.ReadBytes; C.DecodedBytes += Stats.DecodedBytes; C.GetNanoseconds += Stats.DurationNanoseconds;
				if (!Result) CountError(C, Result.error().Code);
				else if (*Result) ++C.Hits; else ++C.Misses;
			});
		}
		auto ObserveCachePut(const FCacheKey& Key, const FCachePutResult& Result, const FCacheStoreStats& Stats, uint64 Nanoseconds) -> void
		{
			Update(Key, [&](auto& C) {
				++C.Puts; C.ReadBytes += Stats.ReadBytes; C.WrittenBytes += Stats.WrittenBytes;
				C.EncodedRawBytes += Stats.CompressedBytes; C.BlobEncodes += Stats.BlobsCompressed; C.BlobsReused += Stats.BlobsReused; C.PutNanoseconds += Nanoseconds;
				if (!Result) { if (Result.error().Code != ECacheError::Canceled) ++C.StoreFailures; CountError(C, Result.error().Code); }
			});
		}
		auto ObserveCache(const FCacheKey& Key, ECacheObservation Event, uint64 Nanoseconds) -> void
		{
			Update(Key, [&](auto& C) {
				switch (Event)
				{
				case ECacheObservation::CapacityRejected: ++C.CapacityRejections; break;
				case ECacheObservation::Fallback: ++C.BuildFallbacks; break;
				case ECacheObservation::SchemaRejected: ++C.SchemaRejections; break;
				case ECacheObservation::RecordRejected: ++C.InvalidRecords; break;
				case ECacheObservation::AsyncStart: ++C.AsyncStarts; C.QueueNanoseconds += Nanoseconds; break;
				}
			});
		}
	}
}
