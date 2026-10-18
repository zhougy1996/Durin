#pragma once

#include "CoreMinimal.h"
#include "DerivedDataCache/DerivedDataCacheTypes.h"

namespace Durin::DerivedData
{
	enum class ECacheMaintenanceOperation : uint8 { Inspect, Verify, Clear };
	struct FCacheMaintenanceOptions
	{
		// Explicit directory only. Does not change the process cache configuration.
		std::filesystem::path Root;
		ECacheMaintenanceOperation Operation = ECacheMaintenanceOperation::Inspect;
		uint32 MaximumEntries = 100000;
		uint64 MaximumFileBytes = 256ull * 1024 * 1024;
		uint64 MaximumReadBytes = 1024ull * 1024 * 1024;
		uint64 MaximumWorkingSetBytes = 512ull * 1024 * 1024;
		std::function<bool()> ShouldCancel;
	};
	struct FCacheMaintenanceReport
	{
		uint64 RecordFiles = 0, RecordBytes = 0, ValueFiles = 0, ValueBytes = 0;
		uint64 ReadBytes = 0, VerifiedFiles = 0, InvalidFiles = 0, UnsupportedFiles = 0, MissingValues = 0;
		uint64 RemovedFiles = 0, RemovedBytes = 0, OtherRootEntries = 0;
		// At most 32 relative-path diagnostics; aggregate counts are not truncated.
		std::vector<std::string> Diagnostics;
		auto IsValid() const -> bool { return InvalidFiles == 0 && UnsupportedFiles == 0 && MissingValues == 0; }
	};
	// Nonblocking exclusive ownership: even inspection refuses a live cache root.
	// Verify never deletes; clear touches only records/ and values/, preserving the
	// ownership file and unknown siblings. Errors/limits/cancellation are explicit.
	DERIVEDDATACACHE_API auto MaintainCacheStorage(const FCacheMaintenanceOptions& Options)
		-> std::expected<FCacheMaintenanceReport, FCacheError>;
}
