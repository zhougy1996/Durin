#include "ShaderCompileService.h"

#include "ShaderCompileUtilities.h"
#include "SlangShaderCompiler.h"
#include "SlangShaderDependencyResolver.h"

#include "Misc/FileFingerprintCache.h"
#include "ShaderCacheStore.h"
#include "Shader/ShaderPaths.h"

namespace Durin
{
	namespace
	{
		template <typename TBuilder>
		auto UpdateHashStringField(TBuilder& Builder, std::string_view Value) -> void
		{
			Builder.UpdateValue(static_cast<uint64>(Value.size()));
			Builder.Update(Value);
		}

		auto BuildRequestKey(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const std::vector<FShaderMacroDefinition>& Macros) -> std::string
		{
			FXxHash128Builder Builder;
			UpdateHashStringField(Builder, "DurinShaderCompileRequest_v1");
			UpdateHashStringField(Builder, VirtualShaderPath);
			UpdateHashStringField(Builder, Options.CompilerEnvironment);
			Builder.UpdateValue(Options.bForceRecompile);
			Builder.UpdateValue(static_cast<uint64>(Options.EntryPoints.size()));
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				UpdateHashStringField(Builder, Options.EntryPoints[Index] ? std::string_view(Options.EntryPoints[Index]) : std::string_view{});
				if (Index < Options.Frequencies.size()) Builder.UpdateValue(Options.Frequencies[Index]);
			}
			Builder.UpdateValue(static_cast<uint64>(Macros.size()));
			for (const FShaderMacroDefinition& Macro : Macros)
			{
				UpdateHashStringField(Builder, Macro.Name);
				Builder.UpdateValue(Macro.HasValue());
				if (Macro.Value) UpdateHashStringField(Builder, *Macro.Value);
			}
			return Builder.Finalize().ToString();
		}

		auto BuildOutputKey(const FShaderVariantKey& VariantKey, const FShaderCompileOptions& Options) -> std::string
		{
			FXxHash128Builder Builder;
			UpdateHashStringField(Builder, "DurinShaderCompileOutput_v1");
			Builder.UpdateValue(VariantKey.Value);
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				UpdateHashStringField(Builder, Options.EntryPoints[Index] ? std::string_view(Options.EntryPoints[Index]) : std::string_view{});
				Builder.UpdateValue(Options.Frequencies[Index]);
			}
			return Builder.Finalize().ToString();
		}

		class FShaderCompileService
		{
		public:
			~FShaderCompileService()
			{
				FileFingerprintCache.Clear();
			}

			auto GetOrCompile(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput
			{
				FShaderCompilerOutput Output;
				if (VirtualShaderPath.empty())
				{
					Output.ErrorMessage = "Virtual shader path is required for shader compile service";
					return Output;
				}
				if (Options.EntryPoints.empty() || Options.EntryPoints.size() != Options.Frequencies.size())
				{
					Output.ErrorMessage = "Shader compile request entry points and frequencies must be non-empty and have matching counts";
					return Output;
				}

				const std::string SourceFilePath = FShaderPaths::SourcePath(VirtualShaderPath);
				FShaderCompileOptions EffectiveOptions = Options;
				EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);
				EffectiveOptions.CompilerEnvironment = Compiler.GetEnvironmentIdentity();

				std::vector<FShaderMacroDefinition> NormalizedMacros;
				if (!ShaderCompileUtilities::NormalizeMacros(EffectiveOptions, NormalizedMacros, Output.ErrorMessage))
				{
					return Output;
				}

				const std::string RequestKey = BuildRequestKey(VirtualShaderPath, EffectiveOptions, NormalizedMacros);
				std::shared_ptr<FInFlightRequest> InFlight;
				bool bOwner = false;
				{
					std::unique_lock Lock(InFlightMutex);
					if (const auto FoundIt = InFlightRequests.find(RequestKey); FoundIt != InFlightRequests.end())
					{
						InFlight = FoundIt->second;
						InFlight->Condition.wait(Lock, [&InFlight] { return InFlight->bCompleted; });
						return InFlight->Output;
					}
					InFlight = std::make_shared<FInFlightRequest>();
					InFlightRequests.emplace(RequestKey, InFlight);
					bOwner = true;
				}

				check(bOwner);
				Output = GetOrCompileInternal(VirtualShaderPath, SourceFilePath, EffectiveOptions, NormalizedMacros);
				{
					std::lock_guard Lock(InFlightMutex);
					InFlight->Output = Output;
					InFlight->bCompleted = true;
					InFlightRequests.erase(RequestKey);
				}
				InFlight->Condition.notify_all();
				return Output;
			}

			auto GetStats() const -> FShaderCompileServiceStats
			{
				return FShaderCompileServiceStats{
					.DependencyResolutions = DependencyResolutions.load(std::memory_order_relaxed),
					.ManifestHits = ManifestHits.load(std::memory_order_relaxed),
					.MemoryHits = MemoryHits.load(std::memory_order_relaxed),
					.DiskHits = DiskHits.load(std::memory_order_relaxed),
					.Compilations = Compilations.load(std::memory_order_relaxed),
					.ContentReads = FileFingerprintCache.GetContentReadCount()
				};
			}

		private:
			struct FInFlightRequest
			{
				std::condition_variable Condition;
				bool bCompleted = false;
				FShaderCompilerOutput Output;
			};

			auto GetOrCompileInternal(
				std::string_view VirtualShaderPath,
				std::string_view SourceFilePath,
				const FShaderCompileOptions& EffectiveOptions,
				const std::vector<FShaderMacroDefinition>& NormalizedMacros
			) -> FShaderCompilerOutput
			{
				FShaderCompilerOutput Output;
				FShaderDependencyKey DependencyKey;
				ShaderCompileUtilities::BuildDependencyKey(VirtualShaderPath, NormalizedMacros, EffectiveOptions.CompilerEnvironment, DependencyKey);

				FShaderMetaData CurrentMetaData;
				bool bManifestCurrent = false;
				if (CacheStore.LoadMetaData(VirtualShaderPath, DependencyKey, CurrentMetaData))
				{
					std::string ManifestError;
					if (!ShaderCompileUtilities::TryReuseMetaData(CurrentMetaData, FileFingerprintCache, bManifestCurrent, ManifestError))
					{
						DURIN_WARN("Failed to validate shader dependency manifest for {}: {}", VirtualShaderPath, ManifestError);
					}
					if (bManifestCurrent) ManifestHits.fetch_add(1, std::memory_order_relaxed);
				}

				if (!bManifestCurrent)
				{
					std::vector<std::string> DependencyPaths;
					std::string DependencyDiagnostics;
					DependencyResolutions.fetch_add(1, std::memory_order_relaxed);
					if (!DependencyResolver.Resolve(SourceFilePath, EffectiveOptions, DependencyPaths, DependencyDiagnostics))
					{
						Output.ErrorMessage = DependencyDiagnostics.empty() ? "Failed to parse shader dependency graph" : DependencyDiagnostics;
						return Output;
					}
					if (!ShaderCompileUtilities::BuildShaderMetaData(DependencyPaths, FileFingerprintCache, CurrentMetaData, Output.ErrorMessage))
					{
						return Output;
					}
					if (!CacheStore.SaveMetaData(VirtualShaderPath, DependencyKey, CurrentMetaData))
					{
						DURIN_WARN("Shader dependency manifest write failed for {}", VirtualShaderPath);
					}
				}

				FShaderVariantKey VariantKey;
				ShaderCompileUtilities::BuildVariantKey(VirtualShaderPath, CurrentMetaData, NormalizedMacros, EffectiveOptions.CompilerEnvironment, VariantKey);
				const std::string OutputKey = BuildOutputKey(VariantKey, EffectiveOptions);

				if (!EffectiveOptions.bForceRecompile)
				{
					std::lock_guard Lock(OutputCacheMutex);
					if (const auto FoundIt = OutputCache.find(OutputKey); FoundIt != OutputCache.end())
					{
						MemoryHits.fetch_add(1, std::memory_order_relaxed);
						return FoundIt->second;
					}
				}

				if (!EffectiveOptions.bForceRecompile && CacheStore.TryLoad(VirtualShaderPath, EffectiveOptions, VariantKey, Output))
				{
					DiskHits.fetch_add(1, std::memory_order_relaxed);
					std::lock_guard Lock(OutputCacheMutex);
					OutputCache.insert_or_assign(OutputKey, Output);
					return Output;
				}

				Compilations.fetch_add(1, std::memory_order_relaxed);
				Output = Compiler.Compile(SourceFilePath, EffectiveOptions);
				if (!Output)
				{
					return Output;
				}

				DURIN_DEBUG("Shader compiled (Virtual: {}, Hash: {})", VirtualShaderPath, VariantKey.Hex);

				if (!CacheStore.Save(VirtualShaderPath, EffectiveOptions, VariantKey, Output))
				{
					DURIN_WARN("Shader compiled successfully, but cache write failed for {}", VirtualShaderPath);
				}
				{
					std::lock_guard Lock(OutputCacheMutex);
					OutputCache.insert_or_assign(OutputKey, Output);
				}

				return Output;
			}

			FSlangShaderCompiler Compiler;
			FSlangShaderDependencyResolver DependencyResolver;
			FShaderCacheStore CacheStore;
			FFileFingerprintCache FileFingerprintCache;
			std::mutex InFlightMutex;
			std::unordered_map<std::string, std::shared_ptr<FInFlightRequest>> InFlightRequests;
			std::mutex OutputCacheMutex;
			std::unordered_map<std::string, FShaderCompilerOutput> OutputCache;
			std::atomic_uint64_t DependencyResolutions = 0;
			std::atomic_uint64_t ManifestHits = 0;
			std::atomic_uint64_t MemoryHits = 0;
			std::atomic_uint64_t DiskHits = 0;
			std::atomic_uint64_t Compilations = 0;
		};

		std::unique_ptr<FShaderCompileService> GShaderCompileService;
	}

	auto InitShaderCompileService() -> void
	{
		GShaderCompileService = std::make_unique<FShaderCompileService>();
	}

	auto ShutdownShaderCompileService() -> void
	{
		GShaderCompileService.reset();
	}

	auto GetOrCompileShader(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput
	{
		if (!GShaderCompileService)
		{
			FShaderCompilerOutput Output;
			Output.ErrorMessage = "Shader compile service is not initialized";
			return Output;
		}

		return GShaderCompileService->GetOrCompile(VirtualShaderPath, Options);
	}

	auto GetShaderCompileServiceStats() -> FShaderCompileServiceStats
	{
		return GShaderCompileService ? GShaderCompileService->GetStats() : FShaderCompileServiceStats{};
	}
}
