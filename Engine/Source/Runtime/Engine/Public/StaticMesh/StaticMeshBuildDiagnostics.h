#pragma once

#include "EngineAPI.h"

namespace Durin
{
	enum class EStaticMeshCacheOperation : uint8 { Read, Decode, Write };
	// Nonfatal cache failure. Clean hits/misses create no error record.
	struct FStaticMeshCacheError
	{
		FStaticMeshCacheError(EStaticMeshCacheOperation InOperation, std::string InMessage)
			: Operation(InOperation), Message(InMessage.substr(0, 960)) {}
		EStaticMeshCacheOperation Operation;
		ENGINE_API auto ToString() const -> std::string;
	private:
		std::string Message;
	};

}
