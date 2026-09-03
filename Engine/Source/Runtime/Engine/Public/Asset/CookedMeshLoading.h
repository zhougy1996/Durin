#pragma once

#include "EngineAPI.h"

namespace Durin
{
	// Separates cooked CPU residency from the independently queued GPU-resource lifecycle.
	enum class ECookedMeshCpuPhase : uint8
	{
		Unloaded,
		IoQueued,
		Reading,
		Decoding,
		CpuReady,
		Failed,
		Cancelled
	};

	enum class ECookedMeshGpuPhase : uint8
	{
		Unavailable,
		Queued,
		Ready,
		Failed
	};

	// One generation-qualified observation returned by blocking and nonblocking mesh APIs.
	struct FCookedMeshLoadStatus
	{
		ECookedMeshCpuPhase CpuPhase = ECookedMeshCpuPhase::Unloaded;
		ECookedMeshGpuPhase GpuPhase = ECookedMeshGpuPhase::Unavailable;
		uint64 Generation = 0;
		uint64 ResourceRevision = 0;

		auto HasCpuData() const -> bool
		{
			return CpuPhase == ECookedMeshCpuPhase::CpuReady && Generation != 0;
		}
	};

	// Reports blocking CPU residency only. Success does not imply GPU readiness;
	// query the mesh render-resource status after explicitly requesting initialization.
	struct FCookedMeshBlockingResult
	{
		FCookedMeshLoadStatus Status;
		std::string Message;

		auto Succeeded() const -> bool { return Status.HasCpuData(); }
		explicit operator bool() const { return Succeeded(); }
	};
}
