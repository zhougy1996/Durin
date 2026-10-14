#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"

namespace Durin
{
	inline constexpr size_t MaximumPhysicsCookDiagnosticBytes = 4096;
	enum class EPhysicsCookStage : uint8 { Input, Cook, Scheduling, Installation };

	struct FPhysicsCookFailure
	{
		ENGINE_API explicit FPhysicsCookFailure(std::string Message, EPhysicsCookStage Stage = EPhysicsCookStage::Cook);
		auto GetStage() const -> EPhysicsCookStage { return Stage; }
		auto ToString() const -> const std::string& { return Message; }
	private:
		std::string Message;
		EPhysicsCookStage Stage;
	};
}
