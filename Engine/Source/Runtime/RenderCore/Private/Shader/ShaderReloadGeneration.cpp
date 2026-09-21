#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		std::atomic_uint64_t GShaderReloadGeneration = 1;
	}

	auto GetShaderReloadGeneration() -> uint64
	{
		return GShaderReloadGeneration.load(std::memory_order_acquire);
	}

	auto AdvanceShaderReloadGeneration() -> uint64
	{
		return GShaderReloadGeneration.fetch_add(
			1, std::memory_order_acq_rel) + 1;
	}

}
