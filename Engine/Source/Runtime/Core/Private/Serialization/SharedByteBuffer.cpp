#include "Serialization/SharedByteBuffer.h"

namespace Durin
{
	auto FSharedByteBuffer::Copy(FByteView Bytes) -> FSharedByteBuffer
	{
		return Take(FByteBuffer(Bytes.begin(), Bytes.end()));
	}

	auto FSharedByteBuffer::Take(FByteBuffer Bytes) -> FSharedByteBuffer
	{
		if (Bytes.empty()) return {};
		return FSharedByteBuffer(std::make_shared<const FByteBuffer>(std::move(Bytes)));
	}
}
