#pragma once

#include "CoreMinimal.h"
#include "Shader/ShaderParameterSchema.h"

namespace Durin::Private
{
	auto BuildRegisteredShaderParameterSchema(const FShaderParametersMetadata& Metadata,
		std::vector<FShaderParameterBinding>& HostBindings) -> std::expected<FShaderParameterSchema, std::string>;

	// Only immutable schemas produced by registration may bypass external-value
	// canonicalization. Resource admission is shared with the public schema path.
	auto BuildShaderParameterMapFromCanonicalSchema(const FShaderParameterSchema& Schema,
		EShaderFrequency Frequency, const FShaderReflectionData& Reflection)
		-> std::expected<FShaderParameterMap, std::string>;
}
