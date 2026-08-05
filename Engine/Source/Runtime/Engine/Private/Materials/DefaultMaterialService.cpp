#include "Materials/DefaultMaterialService.h"

#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Materials/Material.h"
#include "Threading/RunnableThread.h"

#include <array>

namespace Durin
{
	namespace
	{
		struct FDefaultMaterialServiceState
		{
			DMaterial* Material = nullptr;
			FMaterialRenderProxyRef Proxy;
			bool bInitialized = false;
			bool bAcceptingBindings = false;
		};

		FDefaultMaterialServiceState GDefaultMaterialService;
		constexpr std::array<std::string_view, 1> GBuiltInCookRoots{
			DefaultMaterialAssetPath};

		auto CheckDefaultMaterialGameThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}
	}

	auto InitializeDefaultMaterialService() -> bool
	{
		CheckDefaultMaterialGameThread();
		if (GDefaultMaterialService.bInitialized)
		{
			return GDefaultMaterialService.bAcceptingBindings;
		}
		GDefaultMaterialService.bInitialized = true;

		FAssetPath Path;
		if (!FAssetPath::TryCreate(DefaultMaterialAssetPath, Path))
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::DefaultAssetUnavailable);
			DURIN_ERROR_CATEGORY(
				"Material",
				"DefaultAssetUnavailable: Engine default material path '{}' is invalid; ErrorMaterial will be used.",
				DefaultMaterialAssetPath);
			return false;
		}

		DMaterial* Material = nullptr;
		const Asset::FAssetResult LoadResult = Asset::LoadAsset(Path, Material);
		if (!LoadResult || Material == nullptr
			|| Material->GetClass() != DMaterial::StaticClass())
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::DefaultAssetUnavailable);
			DURIN_ERROR_CATEGORY(
				"Material",
				"DefaultAssetUnavailable: failed to load exact DMaterial '{}': {}; ErrorMaterial will be used.",
				DefaultMaterialAssetPath,
				LoadResult.Message.empty()
					? "asset type or object was invalid"
					: LoadResult.Message);
			return false;
		}

		FMaterialRenderProxyRef Proxy = Material->GetMaterialRenderProxy();
		if (!Proxy)
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::DefaultAssetUnavailable);
			DURIN_ERROR_CATEGORY(
				"Material",
				"DefaultAssetUnavailable: material '{}' did not publish a render proxy; ErrorMaterial will be used.",
				DefaultMaterialAssetPath);
			return false;
		}

		AddToRoot(Material);
		GDefaultMaterialService.Material = Material;
		GDefaultMaterialService.Proxy = std::move(Proxy);
		GDefaultMaterialService.bAcceptingBindings = true;
		return true;
	}

	auto ShutdownDefaultMaterialService() -> void
	{
		CheckDefaultMaterialGameThread();
		if (!GDefaultMaterialService.bInitialized) return;
		GDefaultMaterialService.bAcceptingBindings = false;
		GDefaultMaterialService.Proxy = {};
		if (GDefaultMaterialService.Material != nullptr)
		{
			RemoveFromRoot(GDefaultMaterialService.Material);
			GDefaultMaterialService.Material = nullptr;
		}
		GDefaultMaterialService.bInitialized = false;
	}

	auto GetDefaultMaterialRenderProxy() -> FMaterialRenderProxyRef
	{
		CheckDefaultMaterialGameThread();
		return GDefaultMaterialService.bAcceptingBindings
			? GDefaultMaterialService.Proxy
			: FMaterialRenderProxyRef{};
	}

	auto IsDefaultMaterialServiceAvailable() -> bool
	{
		CheckDefaultMaterialGameThread();
		return GDefaultMaterialService.bAcceptingBindings;
	}

	auto GetEngineBuiltInCookRoots() -> std::span<const std::string_view>
	{
		return GBuiltInCookRoots;
	}
}
