#pragma once

#include "CoreMinimal.h"

#include "Modules/ModuleManager.h"
#include "RenderCoreAPI.h"
#include "Shader/ShaderCompilerCore.h"
#include "Shader/ShaderBuildRequest.h"
#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	class FShaderCompilingManager;
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

	private:
		friend class FShaderCompilingManager;
		// Starts work immediately; completion may run before the handle is returned.
		// Public callers submit through FShaderCompilingManager.
		virtual auto CompileMounted(std::string_view VirtualShaderPath,
			const FShaderCompileOptions& CompileOptions, FShaderBuildRequestOptions Options,
			FShaderBuildCompletion Completion)
			-> FShaderBuildHandle = 0;
		virtual auto CompileGenerated(FGeneratedShaderCompileRequest Request,
			FShaderBuildRequestOptions Options, FShaderBuildCompletion Completion)
			-> FShaderBuildHandle = 0;
	public:
		virtual auto GetCompilerEnvironmentIdentity() -> std::string = 0;
		// Capture for exact native-library cache replay through request options.
		virtual auto GetNativeLibraryProducerIdentity() -> std::expected<std::string, std::string>
		{ return std::unexpected("Native shader producer identity is unavailable."); }
		// Target-specific producer provenance is captured before program/Cook reuse.
		// Source-only implementations explicitly reject native library production.
		virtual auto GetTargetProducerIdentity(const FShaderTargetIdentity& Target) -> std::expected<std::string, std::string>
		{
			if (Target == VulkanShaderTarget || Target == MetalShaderTarget) return GetCompilerEnvironmentIdentity();
			return std::unexpected("Native shader producer identity is unavailable.");
		}
		virtual auto BuildSourceDependencyManifest(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult = 0;
		virtual auto BuildSourceTreeFingerprint(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult = 0;
		// Hashes stable mounted sources and compiler settings without retaining file bytes.
		virtual auto GetCookInputIdentity(std::string& OutIdentity, const std::function<bool()>& IsCancelled = {}) -> FShaderCompletion = 0;
		virtual auto GetStats() const -> FShaderBuildStats = 0;
		virtual auto BuildCookedLibrary(
			EShaderTargetPlatform TargetPlatform,
			EShaderTargetProfile TargetProfile,
			FByteBuffer& OutBytes,
			std::shared_ptr<const FShaderSourceArtifacts> Artifacts = {},
			const std::function<bool()>& IsCancelled = {}) -> FShaderCompletion = 0;
	};

	RENDERCORE_API auto GetShaderCookInputIdentity(
		std::string& OutIdentity,
		const std::function<bool()>& IsCancelled = {}) -> FShaderCompletion;

	RENDERCORE_API auto GetShaderBuildStats() -> FShaderBuildStats;
	RENDERCORE_API auto BuildCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		const std::function<bool()>& IsCancelled = {}) -> FShaderCompletion;
}
