#include "Materials/DefaultMaterialService.h"
#include "Misc/UnifiedError.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Asset.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Materials/Material.h"
#include "RenderingThread.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		struct FDefaultMaterialServiceState
		{
			DMaterial* Material = nullptr;
			DMaterial* CubeSky = nullptr;
			FMaterialRenderProxyRef Proxy;
			bool bInitialized = false;
			bool bAcceptingBindings = false;
		};

		FDefaultMaterialServiceState GDefaultMaterialService;
		FMaterialRenderProxyRef GDefaultMaterialProxy_RenderThread;
		FMaterialRenderProxyRef GCubeSkyProxy_RenderThread;
		constexpr std::array<std::string_view, 2> GBuiltInCookRoots{
			DefaultMaterialPackagePath, "/Engine/Materials/DefaultCubeSky"};

		auto CheckDefaultMaterialGameThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto PublishDefaultMaterialProxy(FMaterialRenderProxyRef Proxy, FMaterialRenderProxyRef Sky = {}) -> void
		{
			if (IsRenderingThreadRunning())
				EnqueueRenderCommand("PublishDefaultMaterialProxy", [Proxy = std::move(Proxy), Sky = std::move(Sky)](FRHICommandListImmediate&) {
					GDefaultMaterialProxy_RenderThread = Proxy;
					GCubeSkyProxy_RenderThread = Sky;
				});
		}
	}

	auto InitializeDefaultMaterialService() -> bool
	{
		std::optional<Durin::UnifiedError::FError> LoadError;
		CheckDefaultMaterialGameThread();
		if (GDefaultMaterialService.bInitialized)
		{
			return GDefaultMaterialService.bAcceptingBindings;
		}
		GDefaultMaterialService.bInitialized = true;

		FSoftObjectPath Path;
		if (!FSoftObjectPath::TryCreate(DefaultMaterialObjectPath, Path))
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::DefaultAssetUnavailable);
			DURIN_ERROR_CATEGORY(
				"Material",
				"DefaultAssetUnavailable: Engine default material path '{}' is invalid; ErrorMaterial will be used.",
				DefaultMaterialObjectPath);
			return false;
		}

		const auto Load = LoadObject<DMaterial>(Path, nullptr, &LoadError);
		DMaterial* Material = Load;
		if (Material == nullptr
			|| Material->GetClass() != DMaterial::StaticClass())
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::DefaultAssetUnavailable);
			DURIN_ERROR_CATEGORY(
				"Material",
				"DefaultAssetUnavailable: failed to load exact DMaterial '{}': {}; ErrorMaterial will be used.",
				DefaultMaterialObjectPath,
				bool(Load)
					? "asset type or object was invalid"
					: LoadError->ToString());
			return false;
		}

		(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		if (!Material->GetAcceptedCompiledProgram())
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::DefaultAssetUnavailable);
			DURIN_ERROR_CATEGORY(
				"Material",
				"DefaultAssetUnavailable: material '{}' did not produce a compiled program: {}; ErrorMaterial will be used.",
				DefaultMaterialObjectPath, Material->GetMaterialCookDiagnostic());
			return false;
		}

		FMaterialRenderProxyRef Proxy = Material->GetRenderProxy();
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
		FSoftObjectPath SkyPath;
		FMaterialRenderProxyRef SkyProxy;
		if (FSoftObjectPath::TryCreate("/Engine/Materials/DefaultCubeSky.DefaultCubeSky", SkyPath))
		{
			if (auto* Sky = LoadObject<DMaterial>(SkyPath))
			{
				Sky->CacheResourceShadersForRendering(EMaterialShaderPrecompileMode::Synchronous);
				if (!Sky->GetAcceptedCompiledProgram())
					DURIN_ERROR_CATEGORY("Material", "Default cube sky shader compilation failed: {}", Sky->GetMaterialCookDiagnostic());
				AddToRoot(Sky);
				GDefaultMaterialService.CubeSky = Sky;
				SkyProxy = Sky->GetRenderProxy();
			}
		}
		PublishDefaultMaterialProxy(GDefaultMaterialService.Proxy, std::move(SkyProxy));
		GDefaultMaterialService.bAcceptingBindings = true;
		return true;
	}

	auto ShutdownDefaultMaterialService() -> void
	{
		CheckDefaultMaterialGameThread();
		if (!GDefaultMaterialService.bInitialized) return;
		GDefaultMaterialService.bAcceptingBindings = false;
		PublishDefaultMaterialProxy({});
		GDefaultMaterialService.Proxy = {};
		if (GDefaultMaterialService.CubeSky)
		{
			RemoveFromRoot(GDefaultMaterialService.CubeSky);
			GDefaultMaterialService.CubeSky = nullptr;
		}
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

	auto GetDefaultCubeSkyProxy_RenderThread() -> FMaterialRenderProxyRef
	{
		CheckRenderingThread();
		return GCubeSkyProxy_RenderThread;
	}

	auto GetDefaultMaterialRenderProxy_RenderThread() -> FMaterialRenderProxy*
	{
		CheckRenderingThread();
		return GDefaultMaterialProxy_RenderThread.GetReference();
	}
	auto GetDefaultMaterial() -> DMaterialInterface*
	{
		CheckDefaultMaterialGameThread();
		return GDefaultMaterialService.bAcceptingBindings ? GDefaultMaterialService.Material : nullptr;
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
