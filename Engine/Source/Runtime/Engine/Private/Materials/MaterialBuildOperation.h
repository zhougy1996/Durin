#pragma once
#include "CoreMinimal.h"
#include "MaterialPreparedProgram.h"
#include "Threading/Task.h"

namespace Durin::MaterialBuildPrivate
{
	enum class EMaterialTranslationAction { Compile, Reuse, Join };
	struct FMaterialBuildResult
	{
		FMaterialCompilerResult Program;
		std::vector<MIR::FSource> Sources;
		std::shared_ptr<const FMaterialCompilerResult> Shared;
		bool RetainedHit = false;
	};
	// Engine-owned continuation lifetime. Result readiness and request end differ.
	class ENGINE_API FMaterialBuildOperation : public std::enable_shared_from_this<FMaterialBuildOperation>
	{
	public:
		FMaterialBuildOperation();
		~FMaterialBuildOperation();
		auto Start(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force,
			FTaskCancellationToken Cancellation, std::function<EMaterialTranslationAction(FMaterialTranslatedProgram&)> OnTranslation,
			std::function<void(FMaterialCompilerResult)> Completion,
			std::shared_ptr<const FMaterialTranslationInput> EncodedInput = {}) -> void;
		auto TryReserve() -> bool;
		auto RaisePriority() -> void;
		auto Cancel() -> void;
		auto Wait() -> void;
		auto IsFinished() const -> bool;
		auto GetTaskId() const -> uint64;
	private:
		enum class ECompletionDelivery { Deliver, Handled };
		struct FState;
		std::unique_ptr<FState> State;
		auto Launch(std::function<void()> Work) -> void;
		auto Translated(FMaterialTranslatedProgram Translation) -> void;
		auto Complete(FMaterialCompilerResult Result, ECompletionDelivery Delivery = ECompletionDelivery::Deliver) -> void;
	};
	ENGINE_API auto CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialBuildResult;
}
