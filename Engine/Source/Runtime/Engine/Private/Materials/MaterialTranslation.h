#pragma once
#include "CoreMinimal.h"
#include "MaterialPreparedProgram.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"

namespace Durin::MaterialBuildPrivate
{
	struct FTranslationStats { uint64 Producers = 0, Resolutions = 0, Hits = 0, ProducerMicroseconds = 0, GameThreadProducers = 0; };
	ENGINE_API auto GetTranslationStats() -> FTranslationStats;
	auto ValidateTranslationConsumer(const MIR::FCompilerInput& Input) -> FMaterialError;
	ENGINE_API auto SetTranslationServiceForTests(std::shared_ptr<DerivedData::IBuild> Service) -> void;
	ENGINE_API auto MakeTranslationFunction() -> std::shared_ptr<const DerivedData::IBuildFunction>;
	auto RegisterTranslation() -> void;
	ENGINE_API auto OpenTranslationSessions() -> void;
	ENGINE_API auto CloseTranslationSessions() -> void;
	ENGINE_API auto TranslateMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialTranslatedProgram;
	ENGINE_API auto SubmitTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force,
		DerivedData::IRequestOwner& Owner, std::function<void(FMaterialTranslatedProgram)> Completion,
		DerivedData::FBuildCancellation Cancellation = {}, bool QueryCache = true, std::shared_ptr<const FMaterialTranslationInput> EncodedInput = {},
		DerivedData::EBuildCacheExecution Execution = DerivedData::EBuildCacheExecution::Task)
		-> std::expected<DerivedData::FBuildRequest, DerivedData::FBuildAdmissionError>;
}
#endif
