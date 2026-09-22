#pragma once

#include <string>

#include "EngineAPI.h"

namespace Durin
{
	// Diagnostic location, independent of the compilation lifecycle.
	enum class EStaticMeshBuildStage : uint8
	{
		Build, Source, Render, Collision, Validation, Application, Resources
	};

	// Advanced pipeline failure, shared by detached workers and owner-thread application.
	// Cancellation stays typed here; ordinary async callers use completion Status.
	struct FStaticMeshBuildFailure
	{
		ENGINE_API explicit FStaticMeshBuildFailure(std::string Message,
			EStaticMeshBuildStage InStage = EStaticMeshBuildStage::Build);
		ENGINE_API static auto Cancelled(EStaticMeshBuildStage Stage = EStaticMeshBuildStage::Build,
			std::string Message = "StaticMesh build was cancelled.") -> FStaticMeshBuildFailure;
		auto IsCancelled() const -> bool { return bCancelled; }
		auto GetStage() const -> EStaticMeshBuildStage { return Stage; }
		auto ToString() const -> const std::string& { return Message; }
	private:
		std::string Message;
		EStaticMeshBuildStage Stage;
		bool bCancelled = false;
	};
}
