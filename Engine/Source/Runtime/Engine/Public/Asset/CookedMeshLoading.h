#pragma once

#include "EngineAPI.h"
#include "DObject/ObjectKey.h"

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

	enum class ETaskState : uint8;
	struct FPackageResourceReadResult;
	struct FCookedMeshProductError;
	struct FCookedMeshAdmissionError;
	struct FStaticMeshPublicationError;
	enum class ECookedMeshLoadError : uint8
	{
		None, Unavailable, RenderRead, CollisionRead, Product, Publication,
		Admission, Read, Cancelled, Task, FieldCount, InvalidProduct, CollisionPublication, CompletionBudget
	};
	struct FCookedMeshLoadError
	{
		ECookedMeshLoadError Code = ECookedMeshLoadError::None;
		FObjectKey Owner;
		uint64 Index = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		uint64 Reserved = 0;
		std::optional<ETaskState> TaskState;
		std::shared_ptr<const FPackageResourceReadResult> ReadCause;
		std::shared_ptr<const FCookedMeshProductError> ProductCause;
		std::shared_ptr<const FStaticMeshPublicationError> PublicationCause;
		std::shared_ptr<const FCookedMeshAdmissionError> AdmissionCause;
	};
	struct FCookedMeshLoadResult
	{
		FCookedMeshLoadError Error;
		explicit operator bool() const { return Error.Code == ECookedMeshLoadError::None; }
	};
	ENGINE_API auto FormatCookedMeshLoadError(const FCookedMeshLoadError& Error) -> std::string;

	// Reports blocking CPU residency only. Success does not imply GPU readiness;
	// query the mesh render-resource status after explicitly requesting initialization.
	struct FCookedMeshBlockingResult
	{
		FCookedMeshLoadStatus Status;
		FCookedMeshLoadError Error;

		auto Succeeded() const -> bool { return Error.Code == ECookedMeshLoadError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
}
