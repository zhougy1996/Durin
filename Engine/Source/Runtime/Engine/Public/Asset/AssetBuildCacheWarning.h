#pragma once

#include <string>
#include "EngineAPI.h"

namespace Durin
{
	enum class EAssetBuildCacheOperation : uint8 { Read, Decode, Write };
	// Recoverable cache problems; they do not determine build success.
	struct FAssetBuildCacheWarning
	{
		FAssetBuildCacheWarning(EAssetBuildCacheOperation InOperation, std::string InMessage)
			: Operation(InOperation), Message(InMessage.substr(0, 960)) {}
		EAssetBuildCacheOperation Operation;
		auto ToString() const -> std::string
		{
			const auto Name = Operation == EAssetBuildCacheOperation::Read ? "read"
				: Operation == EAssetBuildCacheOperation::Decode ? "decode" : "write";
			return std::string("cache ") + Name + ": " + Message;
		}
	private:
		std::string Message;
	};
}
