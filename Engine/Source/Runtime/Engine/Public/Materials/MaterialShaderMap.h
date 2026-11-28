#pragma once
#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	class FShaderType;
	class FVertexFactoryType;

	// Owns finalized material program content, shared as const after publication.
	class FMaterialShaderMap final : public FMaterialCompilationOutput
	{
	public:
		ENGINE_API FMaterialShaderMap();
		ENGINE_API ~FMaterialShaderMap();
		// Copies finalized program content.
		ENGINE_API FMaterialShaderMap(const FMaterialShaderMap& Other);
		auto operator=(const FMaterialShaderMap&) -> FMaterialShaderMap& = delete;

		std::vector<FMaterialCompilerDependency> Dependencies;
		std::vector<FShaderCode> CompiledShaders;
		FMaterialProgramIdentity ShaderMapId;
		std::string CompilerIdentity;
		std::string Target;
		uint32 PassContractVersion = CurrentMaterialPassContractVersion;
		FMaterialShaderEntrySet RequiredEntries = GetDefaultMaterialShaderEntries();
		std::shared_ptr<const FMaterialProgramDebugData> DebugData;
		// Elapsed seconds for this map's shader-job stage; retained hits reuse this value.
		float CompileTime = 0;

		// Pass processors supply their selected shader type. Shared vertex entries serve multiple passes.
		ENGINE_API auto FindShaderIndex(const FShaderType& Type, const FVertexFactoryType* Factory,
			uint32 PermutationId) const -> std::optional<size_t>;
		ENGINE_API auto GetShaderMapId() const -> FMaterialProgramIdentity;
		ENGINE_API static auto FindId(FMaterialProgramIdentity Identity) -> std::shared_ptr<const FMaterialShaderMap>;
		ENGINE_API static auto Register(std::shared_ptr<const FMaterialShaderMap> ShaderMap, bool Replace = false)
			-> std::shared_ptr<const FMaterialShaderMap>;
		ENGINE_API static auto ClearCache() -> void;
		ENGINE_API static auto GetCacheStats() -> std::pair<uint32, uint64>;
		ENGINE_API auto GetRetainedBytes() const -> uint64;
		// Called on an unpublished map by assembly or cooked-data decoding.
		ENGINE_API auto SetCollectionLayouts(std::span<const FMaterialParameterCollectionLayout> Collections) -> void;

	};
}
