#include "Physics/PhysicsCookFailure.h"

namespace Durin
{
	FPhysicsCookFailure::FPhysicsCookFailure(std::string InMessage, EPhysicsCookStage InStage)
		: Message(InMessage.substr(0, MaximumPhysicsCookDiagnosticBytes)), Stage(InStage) {}

}
