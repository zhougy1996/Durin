#include "Serialization/SharedByteBuffer.h"

namespace Durin
{
	auto FSharedByteBuffer::Copy(std::span<const std::byte> Bytes) -> FSharedByteBuffer
	{
		return Take(FByteArray(Bytes.begin(), Bytes.end()));
	}

	auto FSharedByteBuffer::Take(FByteArray Bytes) -> FSharedByteBuffer
	{
		if (Bytes.empty()) return {};
		return FSharedByteBuffer(std::make_shared<const FByteArray>(std::move(Bytes)));
	}
}
