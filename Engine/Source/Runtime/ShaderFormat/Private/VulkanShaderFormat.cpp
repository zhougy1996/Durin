#include "VulkanShaderFormat.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ShaderFormat
{
	auto ValidateVulkanCode(FByteView Code) -> bool
	{
		uint32 Magic = 0;
		return Code.size() >= 20 && Code.size() <= 64ull * 1024 * 1024 && Code.size() % 4 == 0
			&& ReadLittleEndianAt(Code, 0, Magic) && Magic == 0x07230203u;
	}
}
