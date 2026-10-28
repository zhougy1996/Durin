#pragma once
#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	// Owns finalized material program content, shared as const after publication.
	class FMaterialShaderMap final
	{
	public:
		ENGINE_API FMaterialShaderMap();
		ENGINE_API ~FMaterialShaderMap();
		// Copies finalized program content.
		ENGINE_API FMaterialShaderMap(const FMaterialShaderMap& Other);
		auto operator=(const FMaterialShaderMap&) -> FMaterialShaderMap& = delete;

		std::vector<FMaterialCompilerParameterDeclaration> ActiveParameters;
		FMaterialRenderLayout Layout;
		FMaterialProgramRequirements Requirements;
		std::vector<FMaterialCompilerDependency> Dependencies;
		std::vector<FShaderCode> CompiledShaders;
		FMaterialProgramIdentity ShaderMapId;
		std::string CompilerIdentity;
		std::string Target;
		uint32 PassContractVersion = CurrentMaterialPassContractVersion;
		uint8 RequiredStages = MaterialAllStageMask;
		// Schemas only: defaults and asset paths belong to each requesting material.
		std::vector<FMaterialParameterCollectionLayout> CollectionLayouts;
		std::shared_ptr<const FMaterialProgramDebugData> DebugData;
		// Elapsed seconds for this map's shader-job stage; retained hits reuse this value.
		float CompileTime = 0;

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
