#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"

namespace Durin
{
	class DMaterialInterface;
	struct FMaterialCompilerResult;
	struct FMaterialProgramPreparationState;
	enum class EMaterialProgramPreparationState : uint8 { Pending, Ready, Failed, Canceled, Stale };
	// Optional cooked-program loading owner. Request/Poll/GetProgram are GameThread
	// operations; Cancel only withdraws this request. No DObject is read on workers.
	class FMaterialProgramPreparation
	{
	public:
		ENGINE_API static auto Request(DMaterialInterface& Material) -> FMaterialProgramPreparation;
		ENGINE_API auto Poll() -> EMaterialProgramPreparationState;
		ENGINE_API auto Cancel() const -> void;
		ENGINE_API auto GetProgram() const -> std::shared_ptr<const FMaterialCompilerResult>;
		ENGINE_API auto GetDiagnostic() const -> std::string;
	private:
		std::shared_ptr<FMaterialProgramPreparationState> State;
	};
}
