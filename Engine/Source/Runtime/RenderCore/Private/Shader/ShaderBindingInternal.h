#pragma once

#include "Shader/Shader.h"

namespace Durin::ShaderPrivate
{
	inline auto AreShaderBindingTypesCompatible(ERHIBindingType ReflectedType, ERHIBindingType ParameterType) -> bool
	{
		if (ReflectedType == ParameterType)
		{
			return true;
		}
		return ReflectedType == ERHIBindingType::UniformBuffer && ParameterType == ERHIBindingType::UniformBufferDynamic;
	}

}
