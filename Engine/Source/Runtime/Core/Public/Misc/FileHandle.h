#pragma once

#include "Misc/CoreTypes.h"
#include "Containers/ContainersFwd.h"

#include "Misc/CoreStd.h"

#include "Misc/FileError.h"

namespace Durin
{
	// Uniquely owned synchronous byte reader; callers serialize access.
	class IFileHandle
	{
	public:
		virtual ~IFileHandle() = default;
		[[nodiscard]] virtual auto GetSize() const -> uint64 = 0;
		// Exact read, serialized by the caller. Failure can modify part of Output.
		[[nodiscard]] virtual auto ReadAt(uint64 Offset, FMutableByteView Output)
			-> std::expected<void, FFileError> = 0;
	};

}
