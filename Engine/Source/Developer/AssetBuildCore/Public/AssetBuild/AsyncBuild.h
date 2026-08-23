#pragma once

#include "AssetBuildCoreAPI.h"

namespace Durin::Asset::Build
{
	// Identifies the terminal outcome of one accepted asynchronous asset build.
	enum class EAsyncBuildStatus : uint8
	{
		Succeeded,
		Failed,
		Canceled,
		Superseded
	};

	struct FAsyncBuildResult
	{
		EAsyncBuildStatus Status = EAsyncBuildStatus::Failed;
		std::string Diagnostic;

		auto Succeeded() const -> bool { return Status == EAsyncBuildStatus::Succeeded; }
	};

	// Accepted requests invoke their completion exactly once on the contributing
	// service's completion thread, including cancellation and supersession.
	using FAsyncBuildCompletion = std::function<void(FAsyncBuildResult)>;
}
