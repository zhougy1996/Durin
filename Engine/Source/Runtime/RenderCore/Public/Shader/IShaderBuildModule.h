#pragma once

#include "CoreMinimal.h"

#include "Modules/ModuleManager.h"
#include "RenderCoreAPI.h"
#include "Shader/ShaderCompilerCore.h"
#include "Shader/ShaderBuildRequest.h"
#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	// Session adapter contracts, independent of producer algorithms and package codecs.
	struct FShaderCompileAdapterVersion
	{
		uint32 FunctionVersion = 0;
		uint32 ConstantsSchema = 0;
		uint32 OutputSchema = 0;
	};

	// Reports live-build cache reuse and actual compiler work.
	struct FShaderBuildStats
	{
		uint64 DependencyResolutions = 0;
		uint64 ManifestHits = 0;
		uint64 MemoryHits = 0;
		uint64 DdcHits = 0;
		uint64 Compilations = 0;
		uint64 ContentReads = 0;
		uint64 OutputEntries = 0;
		uint64 SourceTreeFingerprintHits = 0;
		uint64 ProducerMicroseconds = 0;
		uint64 CacheMicroseconds = 0;
	};

	// Owns every authoring-only Shader source, compiler, manifest, and DDC call.
	class IShaderBuildModule : public IModuleInterface
	{
	public:
		// Borrow the active implementation; consumers drain work before shutdown.
		RENDERCORE_API static auto Get() -> IShaderBuildModule*;
		// Nonzero opaque producer identity, stable until module shutdown; equality only.
		// Includes compiler dependencies and algorithms, independent of payload schemas.
		virtual auto GetBuildVersion() const -> uint64 = 0;
		virtual auto GetCompileAdapterVersion() const -> FShaderCompileAdapterVersion = 0;

		virtual auto CompileMounted(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options) -> FShaderCompilerOutput = 0;
		virtual auto CompileGenerated(
			const FGeneratedShaderCompileRequest& Request)
			-> FShaderCompilerOutput = 0;
		virtual auto SubmitGenerated(FGeneratedShaderCompileRequest Request,
			FShaderBuildRequestOptions Options, FShaderBuildCompletion Completion)
			-> std::expected<FShaderBuildHandle, FShaderBuildAdmissionError> = 0;
		virtual auto GetCompilerEnvironmentIdentity() -> std::string = 0;
		virtual auto BuildSourceDependencyManifest(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult = 0;
		virtual auto BuildSourceTreeFingerprint(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult = 0;
		// Hashes stable mounted sources and compiler settings without retaining file bytes.
		virtual auto GetCookInputIdentity(std::string& OutIdentity, const std::function<bool()>& IsCancelled = {}) -> FShaderOperationResult = 0;
		virtual auto GetStats() const -> FShaderBuildStats = 0;
		virtual auto BuildCookedLibrary(
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			FByteBuffer& OutBytes,
			std::shared_ptr<const FShaderSourceArtifacts> Artifacts = {},
			const std::function<bool()>& IsCancelled = {}) -> FShaderOperationResult = 0;
	};

	RENDERCORE_API auto GetShaderCookInputIdentity(
		std::string& OutIdentity,
		const std::function<bool()>& IsCancelled = {}) -> FShaderOperationResult;

	RENDERCORE_API auto GetShaderBuildStats() -> FShaderBuildStats;
	RENDERCORE_API auto BuildCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		const std::function<bool()>& IsCancelled = {}) -> FShaderOperationResult;
}
