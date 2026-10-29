#include "CacheDispatchPrivate.h"
#pragma once

#include "CoreMinimal.h"
#include "ShaderBuildAPI.h"
#include "DerivedDataBuildSession.h"
#include "ShaderCompileUtilities.h"

namespace Durin
{
	inline constexpr uint32 ShaderCompileBuildFunctionVersion = 5;
	inline constexpr uint32 ShaderCompileConstantsSchemaVersion = 3;
	inline constexpr uint32 ShaderCompileOutputSchemaVersion = 7;
	SHADERBUILD_API auto GetShaderBuildDescriptor() -> DerivedData::FBuildFunctionDescriptor;
	using FShaderArtifactResolver = std::function<std::expected<std::shared_ptr<const FShaderSourceArtifacts>, std::string>()>;
	// Request data lives in constants/request input, never in the registered function.
	struct FShaderSessionRequest
	{
		DerivedData::FBuildDefinition Definition;
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver;
	};
	// Validated recipe; definition encoding and resolver allocation are leader-only.
	struct FShaderSessionRecipe
	{
		FShaderVariantKey Variant;
		uint64 BuilderVersion = 0;
		std::vector<std::string> Roots;
		std::vector<FShaderPortableDependency> Dependencies;
		bool Generated = false;
		FShaderArtifactResolver Resolve;
	};
	SHADERBUILD_API auto PrepareShaderSessionRecipe(const FShaderCompileOptions& NormalizedOptions,
		const FShaderVariantKey& Variant, uint64 BuilderVersion, std::vector<FShaderPortableDependency> Dependencies,
		bool Generated, FShaderArtifactResolver Resolve) -> std::expected<FShaderSessionRecipe, std::string>;
	SHADERBUILD_API auto MakeShaderSessionRequest(const FShaderCompileOptions& NormalizedOptions,
		FShaderSessionRecipe Recipe, std::optional<std::string> GeneratedSource) -> std::expected<FShaderSessionRequest, std::string>;
	SHADERBUILD_API auto MakeShaderSessionRequest(const FShaderCompileOptions& Options,
		const FShaderVariantKey& Variant, uint64 BuilderVersion, std::vector<FShaderPortableDependency> Dependencies,
		std::optional<std::string> GeneratedSource, FShaderArtifactResolver Resolve)
		-> std::expected<FShaderSessionRequest, std::string>;
	// Explicitly constructed before the builder. Close drains all admitted sessions
	// before the module releases compiler services and registration.
	class SHADERBUILD_API FShaderBuildService
	{
	public:
		explicit FShaderBuildService(std::function<void(std::string_view)> BeforeGeneratedCompile = {}, DerivedData::Private::FBuildBackendOptions Options = {});
		~FShaderBuildService();
		auto GetBuildVersion() const -> uint64;
		auto GetCompilerEnvironmentIdentity() const -> const std::string&;
		auto GetProducerMicroseconds() const -> uint64;
		auto GetCacheMicroseconds() const -> uint64;
		auto Execute(FShaderSessionRequest Request, DerivedData::FBuildRequestOptions Options)
			-> DerivedData::FBuildCompleteParams;
		auto Submit(FShaderSessionRequest Request, DerivedData::IRequestOwner& Owner, DerivedData::FBuildCompletionCallback Completion, DerivedData::FBuildRequestOptions Options, bool bInlineExecution) -> std::expected<void, DerivedData::FBuildAdmissionError>;
		auto Close() -> void;
		auto ResetCacheForTests(DerivedData::Private::FBuildBackendOptions Options) -> void;

	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
