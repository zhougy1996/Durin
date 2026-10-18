#pragma once

#include "CoreMinimal.h"
#include "ShaderBuildSession.h"

namespace Durin
{
	inline constexpr uint32 MetalLibraryBuildVersion = 1;
	SHADERBUILD_API auto GetMetalLibraryProducerIdentity(const DerivedData::FBuildCancellation& Cancel = {})
		-> std::expected<std::string, std::string>;
	SHADERBUILD_API auto CreateMetalLibraryBuildFunction() -> std::shared_ptr<const DerivedData::IBuildFunction>;
	SHADERBUILD_API auto MakeMetalLibraryBuildRequest(const FShaderCode& Source,
		std::string ProducerIdentity) -> std::expected<FShaderSessionRequest, std::string>;
}
