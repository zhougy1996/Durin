#include "ShaderBuilder.h"

namespace Durin
{
	FShaderBuilder::FShaderBuilder(std::function<void(std::string_view)> Hook)
		: FShaderBuilder(std::make_shared<FShaderBuildService>(std::move(Hook))) {}

	FShaderBuilder::FShaderBuilder(std::shared_ptr<FShaderBuildService> InService)
		: BuildService(std::move(InService)), CompilerEnvironmentIdentity(BuildService->GetCompilerEnvironmentIdentity()) { SourceTreeFingerprintCacheGeneration = GetShaderReloadGeneration(); InitializeAsync(); }

	FShaderBuilder::~FShaderBuilder()
	{
		Close();
		FileFingerprintCache.Clear();
	}

	auto FShaderBuilder::GetBuildVersion() const -> uint64 { return BuildService->GetBuildVersion(); }

	auto FShaderBuilder::GetCompilerEnvironmentIdentity() const -> const std::string&
	{
		return CompilerEnvironmentIdentity;
	}

	auto FShaderBuilder::GetStats() const -> FShaderBuildStats
	{
		return FShaderBuildStats{
			.DependencyResolutions = DependencyResolutions.load(std::memory_order_relaxed),
			.ManifestHits = ManifestHits.load(std::memory_order_relaxed),
			.MemoryHits = MemoryHits.load(std::memory_order_relaxed),
			.DdcHits = DdcHits.load(std::memory_order_relaxed),
			.Compilations = Compilations.load(std::memory_order_relaxed),
			.ContentReads = FileFingerprintCache.GetContentReadCount(),
			.OutputEntries = OutputCache.Count(),
			.SourceTreeFingerprintHits =
				SourceTreeFingerprintHits.load(std::memory_order_relaxed),
			.ProducerMicroseconds = BuildService->GetProducerMicroseconds(),
			.CacheMicroseconds = BuildService->GetCacheMicroseconds()
		};
	}

}
