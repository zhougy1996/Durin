#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Asset/PackageResourceError.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	enum class ETaskState : uint8;
	struct FCookedMeshAdmissionError;
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
		std::optional<FPackageResourceReadError> ReadCause;
		std::string Diagnostic;
		std::shared_ptr<const FCookedMeshAdmissionError> AdmissionCause;
	};
	struct FCookedMeshLoadResult
	{
		FCookedMeshLoadError Error;
		explicit operator bool() const { return Error.Code == ECookedMeshLoadError::None; }
	};
	ENGINE_API auto FormatCookedMeshLoadError(const FCookedMeshLoadError& Error) -> std::string;

	// CPU residency and the last load error for one asset generation. Execution
	// belongs to the load manager; GPU readiness belongs to GetRenderResourceStatus().
	struct FCookedMeshLoadStatus
	{
		bool bCpuDataResident = false;
		uint64 Generation = 0;
		FCookedMeshLoadError Error;

		auto HasCpuData() const -> bool { return bCpuDataResident && Generation != 0; }
		// Admission failures may be retried when manager capacity becomes available.
		auto HasFailed() const -> bool
		{
			return Error.Code != ECookedMeshLoadError::None
				&& Error.Code != ECookedMeshLoadError::Admission;
		}
	};

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
