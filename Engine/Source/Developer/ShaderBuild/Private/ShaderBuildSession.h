#pragma once
#include "ShaderBuildAPI.h"
#include "DerivedDataBuildSession.h"
#include "ShaderCompileUtilities.h"

namespace Durin
{
	using FShaderArtifactResolver = std::function<std::expected<std::shared_ptr<const FShaderSourceArtifacts>, FShaderError>()>;
	// Request data lives in constants/resolver input, never in the registered function.
	struct FShaderSessionRequest
	{
		DerivedData::FBuildDefinition Definition;
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver;
	};
	SHADERBUILD_API auto MakeShaderSessionRequest(const FShaderCompileOptions& Options,
		const FShaderVariantKey& Variant, std::vector<FShaderPortableDependency> Dependencies,
		std::optional<std::string> GeneratedSource, FShaderArtifactResolver Resolve)
		-> std::expected<FShaderSessionRequest, FShaderError>;
	SHADERBUILD_API auto ShaderSessionError(const DerivedData::FBuildError& Error) -> FShaderError;

	// Explicitly constructed before the builder. Close drains all admitted sessions
	// before the module releases compiler services and registration.
	class SHADERBUILD_API FShaderBuildService
	{
	public:
		explicit FShaderBuildService(std::function<void(std::string_view)> BeforeGeneratedCompile = {});
		~FShaderBuildService();
		auto GetCompilerEnvironmentIdentity() const -> const std::string&;
		auto Execute(FShaderSessionRequest Request, DerivedData::FBuildRequestOptions Options)
			-> std::expected<DerivedData::FBuildCompletion, DerivedData::FBuildError>;
		auto Close() -> void;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
