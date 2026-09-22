#pragma once

#include <string>
#include "EngineAPI.h"

namespace Durin
{
	enum class EPhysicsCacheOperation : uint8 { Read, Decode, Write };
	struct FPhysicsCacheError
	{
		FPhysicsCacheError(EPhysicsCacheOperation InOperation, std::string InMessage)
			: Operation(InOperation), Message(InMessage.substr(0, 960)) {}
		EPhysicsCacheOperation Operation;
		ENGINE_API auto ToString() const -> std::string;
	private:
		std::string Message;
	};
}
