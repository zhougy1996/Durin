#include "Physics/PhysicsCookFailure.h"
#include "Physics/PhysicsCookDiagnostics.h"

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

	auto FPhysicsCacheError::ToString() const -> std::string
	{
		const auto OperationName = Operation == EPhysicsCacheOperation::Read ? "read"
			: Operation == EPhysicsCacheOperation::Decode ? "decode" : "write";
		return std::format("Physics cache {}: {}", OperationName, Message);
	}
}
