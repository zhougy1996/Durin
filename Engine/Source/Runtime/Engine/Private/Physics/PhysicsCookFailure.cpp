#include "Physics/PhysicsCookFailure.h"

namespace Durin
{
	FPhysicsCookFailure::FPhysicsCookFailure(std::string InMessage, EPhysicsCookStage InStage)
		: Message(InMessage.substr(0, MaximumPhysicsCookDiagnosticBytes)), Stage(InStage) {}

	auto FPhysicsCookFailure::Cancelled(EPhysicsCookStage Stage, std::string Message) -> FPhysicsCookFailure
	{
		FPhysicsCookFailure Error(std::move(Message), Stage);
		Error.bCancelled = true;
		return Error;
	}

}
