#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"
#include "Shader/ShaderBuildRequest.h"

namespace Durin
{
	struct FPreparedShaderMapState;

	// Retains exact registered programs while ShaderBuild prepares CPU outputs.
	// Poll never waits and refreshes requests after a shader reload.
	class FShaderMapPreparation
	{
	public:
		RENDERCORE_API static auto Request(std::string_view Owner,
			std::initializer_list<std::string_view> Programs,
			EShaderBuildPriority Priority = EShaderBuildPriority::Normal) -> FShaderMapPreparation;
		RENDERCORE_API auto Poll() -> std::expected<bool, std::string>;
		RENDERCORE_API auto Add(std::string_view Owner,
			std::initializer_list<std::string_view> Programs) -> void;
	private:
		std::vector<std::pair<std::string, std::string>> Programs;
		std::vector<std::shared_ptr<FPreparedShaderMapState>> States;
		std::string Error;
		uint64 Generation = 0;
		uint64 Epoch = 0;
		EShaderBuildPriority Priority = EShaderBuildPriority::Normal;
		auto Refresh() -> void;
	};

	// A thumbnail capture may consume completed outputs, never compile on demand.
	class RENDERCORE_API FPreparedShaderMapScope
	{
	public:
		FPreparedShaderMapScope();
		~FPreparedShaderMapScope();
		FPreparedShaderMapScope(const FPreparedShaderMapScope&) = delete;
		static auto IsActive() -> bool;
		// Counts unavailable CPU outputs encountered in the current outer capture.
		// Resource attempts compare before/after counts to defer only their own work.
		static auto GetPendingRequestCount() -> uint64;
	};

	RENDERCORE_API auto ClearShaderMapPreparations() -> void;
	// Nonblocking invalidation; in-flight results remain private to retired requests.
	RENDERCORE_API auto InvalidateShaderMapPreparations(bool bForceRecompile) -> void;
}
