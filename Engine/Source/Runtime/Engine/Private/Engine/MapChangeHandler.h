#pragma once

#include "CoreMinimal.h"
#include "Engine/MapChange.h"

namespace Durin
{
	class DEngine;
	// Engine-owned preparation. Render completions cannot publish a World.
	class FMapChangeHandler
	{
	public:
		~FMapChangeHandler();
		FMapChangeHandler(const FMapChangeHandler&) = delete;
	private:
		friend class DEngine;
		FMapChangeHandler(DEngine& Host, FMapChangeRequest Request);
		auto Cancel() -> void;
		auto Commit() -> bool;
		auto GetLevel() const -> DLevel*;
		auto GetProgress() const -> FMapChangeProgress;
		auto Advance() -> void;
		auto SubmitView() -> void;
		bool bCommitRequested = false;
		std::function<bool(DLevel&)> BeforeCommit;
		std::function<void(const FMapChangeProgress&)> OnComplete;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
