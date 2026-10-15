#pragma once

#include "CoreMinimal.h"
#include "ShaderBuildAPI.h"

namespace Durin::ShaderBuildPrivate
{
	// Output-affecting revisions belong to the producer, not the runtime codecs.
	inline constexpr uint32 CompileAlgorithmRevision = 5;
	inline constexpr uint32 ReflectionAlgorithmRevision = 3;
	inline constexpr uint32 MetalTranslationAlgorithmRevision = 3;
	inline constexpr uint32 LibraryProductionRevision = 2;

	SHADERBUILD_API auto BuildVersion(std::string_view CompilerEnvironment) -> uint64;
}
