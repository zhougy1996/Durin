#include "Materials/DefaultMaterialService.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Asset.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Materials/Material.h"
#include "Threading/RunnableThread.h"

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
			DefaultMaterialPackagePath};

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

		FObjectPath Path;
		if (!FObjectPath::TryCreate(DefaultMaterialObjectPath, Path))
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::DefaultAssetUnavailable);
			DURIN_ERROR_CATEGORY(
				"Material",
				"DefaultAssetUnavailable: Engine default material path '{}' is invalid; ErrorMaterial will be used.",
				DefaultMaterialObjectPath);
			return false;
		}

		DMaterial* Material = nullptr;
		const FAssetResult LoadResult = LoadObject(Path, Material);
		if (!LoadResult || Material == nullptr
			|| Material->GetClass() != DMaterial::StaticClass())
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::DefaultAssetUnavailable);
			DURIN_ERROR_CATEGORY(
				"Material",
				"DefaultAssetUnavailable: failed to load exact DMaterial '{}': {}; ErrorMaterial will be used.",
				DefaultMaterialObjectPath,
				LoadResult.Message.empty()
					? "asset type or object was invalid"
					: LoadResult.Message);
			return false;
		}

		(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		if (!Material->GetAcceptedCompiledProgram())
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::DefaultAssetUnavailable);
			DURIN_ERROR_CATEGORY(
				"Material",
				"DefaultAssetUnavailable: material '{}' did not produce a compiled program; ErrorMaterial will be used.",
				DefaultMaterialObjectPath);
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
				DefaultMaterialObjectPath);
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
