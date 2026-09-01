#pragma once

#include "RenderCoreAPI.h"

namespace Durin
{
	class FSceneInterface;

	// Transfers final scene destruction to the renderer-provided lifetime policy.
	class FSceneDeleter
	{
	public:
		using FDestroyScene = void (*)(FSceneInterface* Scene);

		constexpr FSceneDeleter() = default;
		explicit constexpr FSceneDeleter(FDestroyScene InDestroyScene)
			: DestroyScene(InDestroyScene)
		{
		}

		auto operator()(FSceneInterface* Scene) const -> void
		{
			checkf(DestroyScene != nullptr,
				"A renderer scene must carry its renderer-provided deleter.");
			DestroyScene(Scene);
		}

	private:
		FDestroyScene DestroyScene = nullptr;
	};

	// Owns the game-thread scene endpoint while allowing deferred render-thread deletion.
	using FScenePtr = std::unique_ptr<FSceneInterface, FSceneDeleter>;
}
