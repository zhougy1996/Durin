#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "DObject/AssetPath.h"
#include "Engine/SceneLoadProgress.h"
#include "SceneView.h"

namespace Durin
{
	class DEngine;
	class DWorld;
	class DLevel;
	class DClass;
	struct FSceneLoadRequest
	{
		FObjectPath Path;
		// An engaged null class requests lifecycle-only BeginPlay.
		std::optional<DClass*> GameMode;
		std::function<std::expected<FSceneView, std::string>(const DWorld&)> BuildInitialView;
	};
	// GameThread-owned operation. Render completions contain no object pins and
	// cannot publish a document. The successful operation retains package residency.
	class FSceneLoadOperation
	{
	public:
		ENGINE_API ~FSceneLoadOperation();
		FSceneLoadOperation(const FSceneLoadOperation&) = delete;
		ENGINE_API auto Cancel() -> void;
		ENGINE_API auto Commit() -> bool;
		ENGINE_API auto GetLevel() const -> DLevel*;
		ENGINE_API auto GetProgress() const -> FSceneLoadProgress;
	private:
		friend class DEngine;
		FSceneLoadOperation(DEngine& Host, FSceneLoadRequest Request, bool bAdmitted);
		auto Advance() -> void;
		auto SubmitView() -> void;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
