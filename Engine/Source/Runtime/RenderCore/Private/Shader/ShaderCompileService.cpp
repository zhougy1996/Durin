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

				const std::string SourceFilePath = FShaderPaths::SourcePath(VirtualShaderPath);
				FShaderCompileOptions EffectiveOptions = Options;
				EffectiveOptions.VirtualShaderPath = std::string(VirtualShaderPath);

				std::vector<FShaderMacroDefinition> NormalizedMacros;
				if (!ShaderCompileUtilities::NormalizeMacros(EffectiveOptions, NormalizedMacros, Output.ErrorMessage))
				{
					return Output;
				}

				std::vector<std::string> DependencyPaths;
				std::string DependencyDiagnostics;
				if (!DependencyResolver.Resolve(SourceFilePath, EffectiveOptions, DependencyPaths, DependencyDiagnostics))
				{
					Output.ErrorMessage = DependencyDiagnostics.empty() ? "Failed to parse shader dependency graph" : DependencyDiagnostics;
					return Output;
				}

				FShaderMetaData CurrentMetaData;
				if (!ShaderCompileUtilities::BuildShaderMetaData(DependencyPaths, FileFingerprintCache, CurrentMetaData, Output.ErrorMessage))
				{
					return Output;
				}

				FShaderVariantKey VariantKey;
				ShaderCompileUtilities::BuildVariantKey(VirtualShaderPath, CurrentMetaData, NormalizedMacros, VariantKey);

				FShaderMetaData CachedMetaData;
				const bool bMetaDataCurrent = CacheStore.LoadMetaData(VirtualShaderPath, CachedMetaData)
					&& ShaderCompileUtilities::IsMetaDataCurrent(CurrentMetaData, CachedMetaData);
				if (!EffectiveOptions.bForceRecompile && bMetaDataCurrent && CacheStore.TryLoad(VirtualShaderPath, EffectiveOptions, VariantKey, Output))
				{
					return Output;
				}

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
				if (!CacheStore.SaveMetaData(VirtualShaderPath, CurrentMetaData))
				{
					DURIN_WARN("Shader compiled successfully, but meta write failed for {}", VirtualShaderPath);
				}

				return Output;
			}

		private:
			FSlangShaderCompiler Compiler;
			FSlangShaderDependencyResolver DependencyResolver;
			FShaderCacheStore CacheStore;
			FFileFingerprintCache FileFingerprintCache;
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
}
