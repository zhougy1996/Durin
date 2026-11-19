#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "RenderResourceCreation.h"
#include "Shader/ShaderBuildRequest.h"

namespace Durin
{
	class FShaderMapBase;
	class FShaderType;
	struct FPreparedShaderMapState;
	// Availability describes this lookup; the compiler only produces terminal outputs.
	struct FShaderMapQueryResult
	{
		ERenderResourceAvailability Status;
		std::string Error;

		FShaderMapQueryResult(ERenderResourceAvailability InStatus = ERenderResourceAvailability::Ready, std::string InError = {})
			: Status(InStatus), Error(std::move(InError))
		{ Error.resize(std::min(Error.size(), ShaderBuildDiagnosticMaxBytes)); }
		FShaderMapQueryResult(std::unexpected<std::string> Failure)
			: FShaderMapQueryResult(ERenderResourceAvailability::Failed, std::move(Failure.error())) {}
		FShaderMapQueryResult(FShaderOperationResult Result)
			: FShaderMapQueryResult(Result ? ERenderResourceAvailability::Ready : ERenderResourceAvailability::Failed,
				Result ? std::string{} : std::move(Result.error())) {}
		explicit operator bool() const { return Status == ERenderResourceAvailability::Ready; }
	};

	// Nonblocking: request shared preparation, or consume only inside capture.
	ENGINE_API auto InitializeShaderMapFromTypes(FShaderMapBase& Map, std::span<const FShaderType* const> Types,
		const FShaderCompileOptions& Options) -> FShaderMapQueryResult;

	// Startup/tools only. Rejects rendering-thread and consumption-only capture calls.
	ENGINE_API auto InitializeShaderMapFromTypesSync(FShaderMapBase& Map, std::span<const FShaderType* const> Types,
		const FShaderCompileOptions& Options) -> FShaderCompletion;

	// Retains exact registered programs while ShaderBuild prepares CPU outputs.
	// Poll never waits and refreshes requests after a shader reload.
	class FShaderMapPreparation
	{
	public:
		ENGINE_API static auto Request(std::string_view Owner,
			std::initializer_list<std::string_view> Programs,
			EShaderBuildPriority Priority = EShaderBuildPriority::Normal) -> FShaderMapPreparation;
		ENGINE_API auto Poll() -> std::expected<bool, std::string>;
		// Resolve only appended programs; invalidation refreshes the complete list.
		ENGINE_API auto Add(std::string_view Owner,
			std::initializer_list<std::string_view> Programs) -> void;
	private:
		std::vector<std::pair<std::string, std::string>> Programs;
		std::vector<std::shared_ptr<FPreparedShaderMapState>> States;
		std::string Error;
		uint64 Generation = 0;
		uint64 Epoch = 0;
		EShaderBuildPriority Priority = EShaderBuildPriority::Normal;
		auto Refresh(size_t FirstProgram = 0) -> void;
	};


	ENGINE_API auto ClearShaderMapPreparations() -> void;
	// Nonblocking invalidation; in-flight results remain private to retired requests.
	ENGINE_API auto InvalidateShaderMapPreparations(bool bForceRecompile) -> void;
}
