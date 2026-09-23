#pragma once

#include "Shader/IShaderBuildModule.h"
#include "ShaderDependencyManifestStore.h"
#include "SlangShaderCompiler.h"
#include "SlangShaderDependencyResolver.h"

namespace Durin
{
	// Module-owned compiler/cache state. Callers drain all work before destruction.
	class FShaderBuilder
	{
	public:
		// Optional compile hook for isolated concurrency tests.
		explicit FShaderBuilder(std::function<void(std::string_view)> InBeforeGeneratedCompile = {});

		~FShaderBuilder();

		auto GetCompilerEnvironmentIdentity() const -> const std::string&;

		auto BuildSourceDependencyManifest(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult;

		auto BuildSourceTreeFingerprint(
			std::string_view VirtualShaderPath,
			const FShaderCompileOptions& Options,
			FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult;

		auto GetOrCompile(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput;

		auto GetStats() const -> FShaderBuildStats;

		auto GetOrCompileGenerated(const FGeneratedShaderCompileRequest& Request)
			-> FShaderCompilerOutput;

	private:
		auto GetOrCompileGeneratedInternal(
			const FGeneratedShaderCompileRequest& Request)
			-> FShaderCompilerOutput;

		static constexpr size_t GMaximumOutputEntries = 128;
		static constexpr size_t GMaximumSourceTreeFingerprintEntries = 128;

		auto TryLoadDerivedData(const FShaderCompileOptions& Options,
			const FShaderVariantKey& VariantKey,
			FShaderCompilerOutput& OutOutput) -> bool;

		auto StoreDerivedData(const FShaderCompileOptions& Options,
			const FShaderVariantKey& VariantKey,
			const FShaderCompilerOutput& Output) -> void;

		static auto ValidateGeneratedImports(
			std::span<const std::string> DependencyPaths,
			std::span<const std::string> AllowedImportVirtualPrefixes) -> FShaderOperationResult;

		struct FOutputCacheEntry
		{
			FShaderCompilerOutput Output;
			std::list<std::string>::iterator Recency;
		};

		auto AddOutput(std::string Key, const FShaderCompilerOutput& Output) -> void;

		struct FInFlightRequest
		{
			std::condition_variable Condition;
			bool bCompleted = false;
			FShaderCompilerOutput Output;
			std::exception_ptr Exception;
		};

		template <typename F>
		auto RunSingleFlight(const std::string& Key, F&& Compile) -> FShaderCompilerOutput
		{
			std::shared_ptr<FInFlightRequest> Flight;
			{
				std::unique_lock Lock(InFlightMutex);
				if (const auto Found = InFlightRequests.find(Key); Found != InFlightRequests.end())
				{
					Flight = Found->second;
					Flight->Condition.wait(Lock, [&] { return Flight->bCompleted; });
					Lock.unlock();
					if (Flight->Exception) std::rethrow_exception(Flight->Exception);
					return Flight->Output;
				}
				Flight = std::make_shared<FInFlightRequest>();
				InFlightRequests.emplace(Key, Flight);
			}
			try { Flight->Output = Compile(); }
			catch (...) { Flight->Exception = std::current_exception(); }
			{
				std::lock_guard Lock(InFlightMutex);
				Flight->bCompleted = true;
				InFlightRequests.erase(Key);
			}
			Flight->Condition.notify_all();
			if (Flight->Exception) std::rethrow_exception(Flight->Exception);
			return Flight->Output;
		}

		auto ResolveSourceMetaData(
			std::string_view VirtualShaderPath,
			std::string_view SourceFilePath,
			const FShaderCompileOptions& EffectiveOptions,
			const std::vector<FShaderMacroDefinition>& NormalizedMacros,
			FShaderMetaData& OutMetaData) -> FShaderOperationResult;

		auto GetOrCompileInternal(
			std::string_view VirtualShaderPath,
			std::string_view SourceFilePath,
			const FShaderCompileOptions& EffectiveOptions,
			const std::vector<FShaderMacroDefinition>& NormalizedMacros
		) -> FShaderCompilerOutput;

		FSlangShaderCompiler Compiler;
		FSlangShaderDependencyResolver DependencyResolver;
		const std::string CompilerEnvironmentIdentity;
		FShaderDependencyManifestStore ManifestStore;
		FFileFingerprintCache FileFingerprintCache;
		std::mutex InFlightMutex;
		std::unordered_map<std::string, std::shared_ptr<FInFlightRequest>> InFlightRequests;
		const std::function<void(std::string_view)> BeforeGeneratedCompile;
		mutable std::mutex OutputCacheMutex;
		std::list<std::string> OutputRecency;
		std::unordered_map<std::string, FOutputCacheEntry> OutputCache;
		std::mutex SourceTreeFingerprintCacheMutex;
		uint64 SourceTreeFingerprintCacheGeneration = 0;
		std::unordered_map<std::string,
			FShaderSourceDependencyFingerprint> SourceTreeFingerprintCache;
		std::atomic_uint64_t DependencyResolutions = 0;
		std::atomic_uint64_t ManifestHits = 0;
		std::atomic_uint64_t MemoryHits = 0;
		std::atomic_uint64_t DdcHits = 0;
		std::atomic_uint64_t DdcCorruptMisses = 0;
		std::atomic_uint64_t DdcStoreFailures = 0;
		std::atomic_uint64_t Compilations = 0;
		std::atomic_uint64_t SourceTreeFingerprintHits = 0;
	};
}
