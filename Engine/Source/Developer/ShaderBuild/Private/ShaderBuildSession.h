#pragma once

#include "CoreMinimal.h"
#include "ShaderBuildAPI.h"
#include "DerivedDataBuildSession.h"
#include "ShaderCompileUtilities.h"

namespace Durin
{
	inline constexpr uint32 ShaderCompileBuildFunctionVersion = 4;
	inline constexpr uint32 ShaderCompileConstantsSchemaVersion = 2;
	inline constexpr uint32 ShaderCompileOutputSchemaVersion = 6;
	SHADERBUILD_API auto GetShaderBuildDescriptor() -> DerivedData::FBuildFunctionDescriptor;
	using FShaderArtifactResolver = std::function<std::expected<std::shared_ptr<const FShaderSourceArtifacts>, FShaderError>()>;
	// Request data lives in constants/request input, never in the registered function.
	struct FShaderSessionRequest
	{
		DerivedData::FBuildDefinition Definition;
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver;
	};
	SHADERBUILD_API auto MakeShaderSessionRequest(const FShaderCompileOptions& Options,
		const FShaderVariantKey& Variant, uint64 BuilderVersion, std::vector<FShaderPortableDependency> Dependencies,
		std::optional<std::string> GeneratedSource, FShaderArtifactResolver Resolve)
		-> std::expected<FShaderSessionRequest, FShaderError>;
	// Explicitly constructed before the builder. Close drains all admitted sessions
	// before the module releases compiler services and registration.
	class SHADERBUILD_API FShaderBuildService
	{
	public:
		explicit FShaderBuildService(std::function<void(std::string_view)> BeforeGeneratedCompile = {},
			DerivedData::FBuildServiceOptions Options = {});
		~FShaderBuildService();
		auto GetBuildVersion() const -> uint64;
		auto GetCompilerEnvironmentIdentity() const -> const std::string&;
		auto GetProducerMicroseconds() const -> uint64;
		auto GetCacheMicroseconds() const -> uint64;
		auto Execute(FShaderSessionRequest Request, DerivedData::FBuildRequestOptions Options)
			-> DerivedData::FBuildCompleteParams;
		auto Submit(FShaderSessionRequest Request, DerivedData::IRequestOwner& Owner,
			DerivedData::FBuildCompletionCallback Completion, DerivedData::FBuildRequestOptions Options,
			bool bInlineExecution) -> std::expected<DerivedData::FBuildRequest, DerivedData::FBuildAdmissionError>;
		auto Close() -> void;
		auto ResetCacheForTests(DerivedData::FBuildServiceOptions Options) -> void;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
