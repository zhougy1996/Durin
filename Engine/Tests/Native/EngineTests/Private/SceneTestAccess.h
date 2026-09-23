#pragma once

#include "Scene.h"
#include "SceneOwnership.h"
#include "Math/Operations.h"

namespace Durin
{
	// Keeps renderer proxy mutation available to contract tests without widening
	// the production component-level scene boundary.
	class FSceneInterfaceTestAccess final
	{
	public:
		static auto CreateScene() -> FScenePtr
		{
			return FScenePtr(new FScene(), FSceneDeleter(&FScene::DestroyScene));
		}

		static auto ReleaseScene(FScenePtr& Scene) -> void
		{
			if (!Scene) return;
			Scene->Release();
			Scene.reset();
		}

		static auto AddPrimitiveProxy(FScene& Scene, FPrimitiveComponentId Id, std::unique_ptr<FPrimitiveSceneProxy> Proxy, const FMatrix& Transform, bool bVisible = true) -> void
		{
			Scene.AddPrimitiveProxy(
				Id, std::move(Proxy), Transform, bVisible
			);
		}

		static auto ReplacePrimitiveProxy(FScene& Scene, FPrimitiveComponentId Id,
			std::unique_ptr<FPrimitiveSceneProxy> Proxy,
			const FMatrix& Transform, bool bVisible = true) -> bool
		{
			if (Id == InvalidPrimitiveComponentId || Proxy == nullptr
				|| !Math::IsFinite(Transform)) return false;
			Scene.RemovePrimitiveProxy(Id);
			Scene.AddPrimitiveProxy(
				Id, std::move(Proxy), Transform, bVisible);
			return true;
		}

		static auto RemovePrimitiveProxy(
			FScene& Scene, FPrimitiveComponentId Id
		) -> void
		{
			Scene.RemovePrimitiveProxy(Id);
		}

		static auto TryAddLightProxy(FScene& Scene, std::unique_ptr<FLightSceneProxy> Proxy) -> bool
		{
			return Scene.TryAddLightProxy(std::move(Proxy));
		}

		static auto TryRemoveLightProxy(
			FScene& Scene, FLightSceneProxy* Proxy
		) -> bool
		{
			return Scene.TryRemoveLightProxy(Proxy);
		}

		static auto TryAddSkyBoxProxy(FScene& Scene, std::unique_ptr<FSkyBoxSceneProxy> Proxy) -> bool
		{
			return Scene.TryAddSkyBoxProxy(std::move(Proxy));
		}

		static auto TryRemoveSkyBoxProxy(
			FScene& Scene, FSkyBoxSceneProxy* Proxy
		) -> bool
		{
			return Scene.TryRemoveSkyBoxProxy(Proxy);
		}

		static auto TryAddVolumetricCloudProxy(FScene& Scene, std::unique_ptr<FVolumetricCloudSceneProxy> Proxy) -> bool
		{
			return Scene.TryAddVolumetricCloudProxy(std::move(Proxy));
		}

		static auto TryRemoveVolumetricCloudProxy(FScene& Scene, FVolumetricCloudSceneProxy* Proxy) -> bool
		{
			return Scene.TryRemoveVolumetricCloudProxy(Proxy);
		}
	};

	class FSceneTestOwner final
	{
	public:
		FSceneTestOwner()
			: Scene(FSceneInterfaceTestAccess::CreateScene())
		{
		}
		~FSceneTestOwner()
		{
			FSceneInterfaceTestAccess::ReleaseScene(Scene);
		}

		FSceneTestOwner(const FSceneTestOwner&) = delete;
		auto operator=(const FSceneTestOwner&) -> FSceneTestOwner& = delete;

		auto Get() const -> FScene*
		{
			return static_cast<FScene*>(Scene.get());
		}
		auto operator*() const -> FScene& { return *Get(); }
		auto operator->() const -> FScene* { return Get(); }
		auto Reset() -> void
		{
			FSceneInterfaceTestAccess::ReleaseScene(Scene);
		}

	private:
		FScenePtr Scene;
	};
} // namespace Durin
