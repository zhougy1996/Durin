#pragma once

#include "CoreMinimal.h"

#include "Shader/IShaderBuildModule.h"
#include "ShaderDependencyManifestStore.h"
#include "ShaderBuildSession.h"
#include "ShaderOutputCache.h"
#include "SlangShaderDependencyResolver.h"

namespace Durin
{
	// Module-owned compiler/cache state. Callers drain all work before destruction.
	class FShaderBuilder
	{
	public:
		// Optional compile hook for isolated concurrency tests.
		explicit FShaderBuilder(std::function<void(std::string_view)> InBeforeGeneratedCompile = {});

		explicit FShaderBuilder(std::shared_ptr<FShaderBuildService> InBuildService);

		~FShaderBuilder();

		auto GetBuildVersion() const -> uint64;

		auto GetCompilerEnvironmentIdentity() const -> const std::string&;

		auto BuildSourceDependencyManifest(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult;

		auto BuildSourceTreeFingerprint(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult;

		auto GetOrCompile(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderBuildResult;

		auto GetStats() const -> FShaderBuildStats;

		auto GetOrCompileGenerated(const FGeneratedShaderCompileRequest& Request)
			-> FShaderBuildResult;
		auto SubmitGenerated(FGeneratedShaderCompileRequest Request, FShaderBuildRequestOptions Options,
			FShaderBuildCompletion Completion) -> std::expected<FShaderBuildHandle, FShaderBuildAdmissionError>;
		auto Close() -> void;

	private:
		enum class ESourceKind { Mounted, Generated };
		struct FPreparedShaderBuild
		{
			std::shared_ptr<const FGeneratedShaderCompileRequest> Request;
			FShaderCompileOptions Options;
			FShaderSessionRecipe Recipe;
			std::string OutputKey;
		};
		auto PrepareGenerated(std::shared_ptr<const FGeneratedShaderCompileRequest> Request,
			const std::function<bool(uint64)>& ReserveSources)
			-> std::expected<std::shared_ptr<FPreparedShaderBuild>, FShaderError>;
		auto PrepareMounted(std::shared_ptr<const FGeneratedShaderCompileRequest> Request,
			const std::function<bool(uint64)>& ReserveSources) -> std::expected<std::shared_ptr<FPreparedShaderBuild>, FShaderError>;
		auto SubmitRequest(FGeneratedShaderCompileRequest Request, FShaderBuildRequestOptions Options,
			FShaderBuildCompletion Completion, ESourceKind SourceKind) -> std::expected<FShaderBuildHandle, FShaderBuildAdmissionError>;
		struct FAsyncState;
		std::shared_ptr<FAsyncState> Async;
		auto InitializeAsync() -> void;

		static constexpr size_t GMaximumSourceTreeFingerprintEntries = 128;

		static auto ValidateGeneratedImports(
			std::span<const std::string> DependencyPaths,
			std::span<const std::string> AllowedImportVirtualPrefixes) -> FShaderOperationResult;

		auto ResolveSourceMetaData(
			std::string_view VirtualShaderPath,
			std::string_view SourceFilePath,
			const FShaderCompileOptions& EffectiveOptions,
			const std::vector<FShaderMacroDefinition>& NormalizedMacros,
			FShaderMetaData& OutMetaData, bool Fresh = false) -> FShaderOperationResult;

		std::shared_ptr<FShaderBuildService> BuildService;
		FSlangShaderDependencyResolver DependencyResolver;
		const std::string CompilerEnvironmentIdentity;
		FShaderDependencyManifestStore ManifestStore;
		FFileFingerprintCache FileFingerprintCache;
		std::mutex SourcePreparationMutex;
		std::unordered_map<std::string, std::weak_ptr<std::mutex>> SourcePreparationLocks;
		FShaderOutputCache OutputCache;
		std::mutex SourceTreeFingerprintCacheMutex;
		uint64 SourceTreeFingerprintCacheGeneration = 0;
		std::unordered_map<std::string,
			FShaderSourceDependencyFingerprint> SourceTreeFingerprintCache;
		std::atomic_uint64_t DependencyResolutions = 0;
		std::atomic_uint64_t ManifestHits = 0;
		std::atomic_uint64_t MemoryHits = 0;
		std::atomic_uint64_t DdcHits = 0;
		std::atomic_uint64_t Compilations = 0;
		std::atomic_uint64_t SourceTreeFingerprintHits = 0;
	};
}
