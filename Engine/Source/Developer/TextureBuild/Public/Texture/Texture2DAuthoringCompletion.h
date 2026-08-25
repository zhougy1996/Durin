#pragma once

#include "TextureBuildAPI.h"

namespace Durin::Asset::Build
{
	// Identifies the terminal outcome of one accepted asynchronous asset build.
	enum class ETexture2DAuthoringStatus : uint8
	{
		Succeeded,
		Failed,
		Canceled,
		Superseded
	};

	struct FTexture2DAuthoringResult
	{
		ETexture2DAuthoringStatus Status = ETexture2DAuthoringStatus::Failed;
		std::string Diagnostic;

		auto Succeeded() const -> bool { return Status == ETexture2DAuthoringStatus::Succeeded; }
	};

	// Accepted requests invoke their completion exactly once on the contributing
	// service's completion thread, including cancellation and supersession.
	using FTexture2DAuthoringCompletion = std::function<void(FTexture2DAuthoringResult)>;
}
