#include "Serialization/SharedByteBuffer.h"

namespace Durin
{
	auto FSharedByteBuffer::Copy(std::span<const std::byte> Bytes) -> FSharedByteBuffer
	{
		return Take(std::vector<std::byte>(Bytes.begin(), Bytes.end()));
	}

	auto FSharedByteBuffer::Take(std::vector<std::byte> Bytes) -> FSharedByteBuffer
	{
		if (Bytes.empty()) return {};
		return FSharedByteBuffer(std::make_shared<const std::vector<std::byte>>(std::move(Bytes)));
	}
}
