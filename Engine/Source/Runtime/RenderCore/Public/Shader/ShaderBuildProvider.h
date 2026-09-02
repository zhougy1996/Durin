#pragma once

#include "Modules/ModularFeature.h"
#include "RenderCoreAPI.h"
#include "Shader/ShaderCompilerCore.h"
#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	// Reports live-build cache reuse and actual compiler work.
	struct FShaderBuildStats
	{
		uint64 DependencyResolutions = 0;
		uint64 ManifestHits = 0;
		uint64 MemoryHits = 0;
		uint64 DdcHits = 0;
		uint64 DdcCorruptMisses = 0;
		uint64 DdcStoreFailures = 0;
		uint64 Compilations = 0;
		uint64 ContentReads = 0;
		uint64 OutputEntries = 0;
		uint64 SourceTreeFingerprintHits = 0;
	};

	// Owns every authoring-only Shader source, compiler, manifest, and DDC call.
	class IShaderBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName =
			"RenderCore.ShaderBuildProvider";
		static constexpr uint32 FeatureVersion = 1;

		virtual auto CompileMounted(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options) -> FShaderCompilerOutput = 0;
		virtual auto CompileGenerated(
			const FGeneratedShaderCompileRequest& Request)
			-> FShaderCompilerOutput = 0;
		virtual auto GetCompilerEnvironmentIdentity() -> std::string = 0;
		virtual auto BuildSourceDependencyManifest(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
			std::string& OutError) -> bool = 0;
		virtual auto BuildSourceTreeFingerprint(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			FShaderSourceDependencyFingerprint& OutFingerprint,
			std::string& OutError) -> bool = 0;
		virtual auto GetStats() const -> FShaderBuildStats = 0;
		virtual auto BuildCookedLibrary(
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			FByteArray& OutBytes,
			std::string& OutError) -> bool = 0;
	};

	RENDERCORE_API auto IsShaderBuildProviderAvailable() -> bool;
	RENDERCORE_API auto GetShaderBuildStats() -> FShaderBuildStats;
	RENDERCORE_API auto BuildCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteArray& OutBytes,
		std::string& OutError) -> bool;
}
