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
		static constexpr uint32 FeatureVersion = 3;

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
		virtual auto CaptureSourceArtifacts(
			std::shared_ptr<const FShaderSourceArtifacts>& OutArtifacts,
			std::string& OutError, const std::function<bool()>& IsCancelled = {}) -> bool
		{
			OutArtifacts.reset();
			OutError = "Shader provider does not support fixed source capture.";
			return false;
		}
		virtual auto GetStats() const -> FShaderBuildStats = 0;
		virtual auto BuildCookedLibrary(
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			FByteBuffer& OutBytes,
			std::string& OutError,
			std::shared_ptr<const FShaderSourceArtifacts> Artifacts = {},
			const std::function<bool()>& IsCancelled = {}) -> bool = 0;
	};

	// Retains one provider invocation through Work; nested shader calls use that
	// provider even if its registration retires. Nested capture visitors fail.
	RENDERCORE_API auto WithShaderBuildProvider(
		const std::function<bool(IShaderBuildProvider&)>& Work,
		std::string& OutError) -> bool;

	// Acquires mounted source bytes once, then binds all synchronous shader work
	// in Work to them and to one retained provider. Acquisition requires quiescent sources.
	// Fixed source/search-root/compiler identity, available only inside a captured invocation.
	RENDERCORE_API auto GetCapturedShaderBuildIdentity() -> std::string;
	RENDERCORE_API auto WithCapturedShaderBuildInputs(
		const std::function<bool()>& Work, std::string& OutError,
		EShaderTargetPlatform TargetPlatform = EShaderTargetPlatform::Win64,
		EShaderTargetProfile TargetProfile = EShaderTargetProfile::Game,
		const std::function<bool()>& IsCancelled = {}) -> bool;

	RENDERCORE_API auto IsShaderBuildProviderAvailable() -> bool;
	RENDERCORE_API auto GetShaderBuildStats() -> FShaderBuildStats;
	RENDERCORE_API auto BuildCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		std::string& OutError) -> bool;
}
