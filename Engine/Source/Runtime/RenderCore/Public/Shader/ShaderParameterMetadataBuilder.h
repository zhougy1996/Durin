#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"
#include "RHIShaderParameters.h"
#include "Shader/ShaderDiagnostics.h"

namespace Durin
{
	// Builds data-driven structures using the same metadata as typed shader parameters.
	class FShaderParametersMetadataBuilder
	{
	public:
		RENDERCORE_API auto AddResourceParameter(std::string_view Name, ERHIBindingType Type,
			uint32 ArraySize = 1, bool bOptional = false) -> FShaderOperationResult;
		// Referenced nested metadata must outlive the built structure.
		RENDERCORE_API auto AddNestedStruct(std::string_view Name, const FShaderParametersMetadata& Metadata)
			-> FShaderOperationResult;
		RENDERCORE_API auto Build(std::string_view StructName) const
			-> std::expected<std::shared_ptr<const FShaderParametersMetadataStorage>, std::string>;
	private:
		auto AddMember(std::string_view Name, FShaderParameterMemberMetadata Member, uint32 Alignment)
			-> FShaderOperationResult;
		std::vector<std::string> Names;
		std::vector<FShaderParameterMemberMetadata> Members;
		uint32 NextOffset = 0;
		uint32 StructAlignment = 1;
	};
}
