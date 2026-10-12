#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"

namespace Durin
{
	namespace TexturePrivate
	{
		auto RegisterBuildFunctions() -> void;
		auto ShutdownBuildSessions() -> void;
		auto BuildAsync(DerivedData::FBuildDefinition Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
			DerivedData::IRequestOwner& Owner,
			DerivedData::FBuildCompletionCallback Completion,
			DerivedData::FBuildRequestOptions Options = {}) -> std::expected<void, DerivedData::FBuildAdmissionError>;
		auto Build(DerivedData::FBuildDefinition Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
			DerivedData::FBuildRequestOptions Options = {})
			-> std::expected<DerivedData::FBuildCompleteParams, DerivedData::FBuildAdmissionError>;
	}
}
#endif
