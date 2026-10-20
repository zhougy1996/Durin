#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialShaderMap.h"
#include "Threading/CancellationToken.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin { struct FMaterialTranslationInput; }

namespace Durin::MaterialBuildPrivate
{

	using FMaterialProgramCompileHandle = std::shared_ptr<FMaterialShaderMap>;

	struct FMaterialBuildResult
	{
		std::shared_ptr<const FMaterialCompilerResult> Program;
		std::vector<MIR::FSource> Sources;
		std::vector<uint32> SourceOrdinals;
		bool RetainedHit = false;
		bool SingleFlight = false;
		explicit operator bool() const { return Program && bool(*Program); }
	};

	enum class EMaterialCompileAdmissionError { Capacity, InputTooLarge, Closed, Unavailable };
	struct FMaterialProgramCompileOptions
	{
		bool bForceRecompile = false;
		FCancellationToken Cancellation;
		std::shared_ptr<const FMaterialTranslationInput> EncodedInput;
		std::shared_ptr<FMaterialShaderMap> ShaderMap;
	};
	ENGINE_API auto SetMaterialBuildCompletionHookForTests(std::function<void(EMaterialQualityLevel, bool)> Hook) -> void;
	struct FMaterialProgramCompileStats { uint64 Joins = 0; };
	ENGINE_API auto GetMaterialProgramCompileStatsForTests() -> FMaterialProgramCompileStats;
	ENGINE_API auto CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialBuildResult;
}
