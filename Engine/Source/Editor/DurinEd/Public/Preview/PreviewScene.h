#pragma once

#include "DurinEdAPI.h"
#include "DObject/ObjectPtr.h"

namespace Durin
{
	class DLevel;
	class DWorld;
	class IScene;

	// Owns an editor-only renderer scene and the ordinary preview world that feeds it.
	class FPreviewScene
	{
	public:
		DURINED_API explicit FPreviewScene(FName Name);
		DURINED_API ~FPreviewScene();

		FPreviewScene(const FPreviewScene&) = delete;
		FPreviewScene& operator=(const FPreviewScene&) = delete;

		auto IsAvailable() const -> bool { return Error.empty() && RenderScene != nullptr && World != nullptr; }
		auto GetDiagnostic() const -> const std::string& { return Error; }
		auto GetRenderScene() const -> IScene* { return RenderScene.get(); }
		auto GetWorld() const -> DWorld* { return World.Get(); }
		auto GetLevel() const -> DLevel* { return Level.Get(); }

		DURINED_API auto BeginPlay() -> void;
		DURINED_API auto Tick(float DeltaSeconds) -> void;
		DURINED_API auto EndPlay() -> void;

	private:
		std::unique_ptr<IScene> RenderScene;
		TObjectPtr<DWorld> World;
		TObjectPtr<DLevel> Level;
		std::string Error;
	};
} // namespace Durin
