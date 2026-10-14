#pragma once
#include "CoreMinimal.h"
#include "MaterialPreparedProgram.h"
#include "Threading/Task.h"

namespace Durin::MaterialBuildPrivate
{
	// Engine-owned continuation lifetime. Business completion and retirement differ.
	class ENGINE_API FMaterialBuildOperation : public std::enable_shared_from_this<FMaterialBuildOperation>
	{
	public:
		FMaterialBuildOperation();
		~FMaterialBuildOperation();
		auto Start(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force,
			FTaskCancellationToken Cancellation, std::function<bool(FMaterialTranslatedProgram&)> OnTranslation,
			std::function<void(FMaterialCompilerResult)> Completion) -> void;
		auto TryReserve() -> bool;
		auto RaisePriority() -> void;
		auto Cancel() -> void;
		auto Wait() -> void;
		auto IsRetired() const -> bool;
		auto GetTaskId() const -> uint64;
	private:
		struct FState;
		std::unique_ptr<FState> State;
		auto Launch(std::function<void()> Work) -> void;
		auto Translated(FMaterialTranslatedProgram Translation) -> void;
		auto Complete(FMaterialCompilerResult Result, bool Deliver = true) -> void;
	};
	ENGINE_API auto CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force,
		std::vector<MIR::FSource>* Sources = nullptr, std::shared_ptr<const FMaterialCompilerResult>* Shared = nullptr, bool* RetainedHit = nullptr) -> FMaterialCompilerResult;
}
