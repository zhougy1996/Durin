#include "Materials/DefaultMaterialService.h"
#include "Misc/UnifiedError.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Asset.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Materials/Material.h"
#include "Materials/MaterialShaderMap.h"
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
			DMaterial* ProceduralSky = nullptr;
			FMaterialRenderProxyRef Proxy;
			bool bInitialized = false;
			bool bAcceptingBindings = false;
		};

		FDefaultMaterialServiceState GDefaultMaterialService;
		FMaterialRenderProxyRef GDefaultMaterialProxy_RenderThread;
		FMaterialRenderProxyRef GCubeSkyProxy_RenderThread;
		FMaterialRenderProxyRef GProceduralSkyProxy_RenderThread;
		std::optional<FDefaultCubeSkyBindings> GCubeSkyBindings_RenderThread;
		FMaterialRenderLayoutIdentity GCubeSkyLayout_RenderThread;

		auto ResolveCubeSkyBindings(const DMaterialInterface& Material, const FMaterialRenderLayout& Layout)
			-> std::optional<FDefaultCubeSkyBindings>
		{
			auto Find = [&](const char* Name, EMaterialRenderValueType Type) -> const FMaterialRenderField*
			{
				const auto* Parameter = Material.FindParameterDefinition(FName(Name));
				if (Parameter)
				{
					const auto Field = std::ranges::find(Layout.Fields, Parameter->Id, &FMaterialRenderField::ParameterId);
					if (Field != Layout.Fields.end() && Field->Type == Type) return &*Field;
				}
				DURIN_ERROR_CATEGORY("Material", "DefaultCubeSky requires active parameter '{}' with render type {}.", Name, static_cast<int>(Type));
				return nullptr;
			};
			const auto* Texture = Find(FDefaultCubeSkyBindings::TextureName, EMaterialRenderValueType::TextureCube);
			const auto* Tint = Find(FDefaultCubeSkyBindings::TintName, EMaterialRenderValueType::Vector4);
			const auto* Intensity = Find(FDefaultCubeSkyBindings::IntensityName, EMaterialRenderValueType::Scalar);
			if (!Texture || !Tint || !Intensity) return std::nullopt;
			return FDefaultCubeSkyBindings{Texture->CompactIndex, Tint->Offset, Intensity->Offset};
		}

		constexpr std::array<std::string_view, 3> GBuiltInCookRoots{
			DefaultMaterialPackagePath, "/Engine/Materials/DefaultCubeSky", "/Engine/Materials/DefaultProceduralSky"};

		auto CheckDefaultMaterialGameThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto PublishDefaultMaterialProxy(FMaterialRenderProxyRef Proxy, FMaterialRenderProxyRef Sky = {},
			FMaterialRenderProxyRef Procedural = {}) -> void
		{
			if (IsRenderingThreadRunning())
				EnqueueRenderCommand("PublishDefaultMaterialProxy", [Proxy = std::move(Proxy), Sky = std::move(Sky), Procedural = std::move(Procedural)](FRHICommandListImmediate&) {
					GDefaultMaterialProxy_RenderThread = Proxy;
					GCubeSkyProxy_RenderThread = Sky;
					GProceduralSkyProxy_RenderThread = Procedural;
				});
		}
	}

	auto RefreshDefaultCubeSkyBindings_GameThread(const DMaterialInterface& Material, const FMaterialProgram* Program) -> void
	{
		if (&Material != GDefaultMaterialService.CubeSky) return;
		const auto* Layout = Program ? &Program->GetShaderMap().Layout : nullptr;
		auto Bindings = Layout ? ResolveCubeSkyBindings(Material, *Layout) : std::nullopt;
		EnqueueRenderCommand("PublishDefaultCubeSkyBindings",
			[Bindings, Identity = Layout ? Layout->Identity : FMaterialRenderLayoutIdentity{}](FRHICommandListImmediate&) {
				GCubeSkyBindings_RenderThread = Bindings;
				GCubeSkyLayout_RenderThread = Identity;
			});
	}

	auto GetDefaultCubeSkyBindings_RenderThread(const FMaterialRenderLayoutIdentity& Layout) -> const FDefaultCubeSkyBindings*
	{
		CheckRenderingThread();
		return GCubeSkyBindings_RenderThread && Layout == GCubeSkyLayout_RenderThread ? &*GCubeSkyBindings_RenderThread : nullptr;
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
		auto LoadSky = [](std::string_view ObjectPath, DMaterial*& Sky) -> FMaterialRenderProxyRef
		{
			FSoftObjectPath SkyPath;
			if (!FSoftObjectPath::TryCreate(ObjectPath, SkyPath)) return {};
			Sky = LoadObject<DMaterial>(SkyPath);
			if (!Sky) return {};
			AddToRoot(Sky);
			Sky->CacheResourceShadersForRendering(EMaterialShaderPrecompileMode::Synchronous);
			if (!Sky->GetAcceptedCompiledProgram())
				DURIN_ERROR_CATEGORY("Material", "Sky material '{}' compilation failed: {}", ObjectPath, Sky->GetMaterialCookDiagnostic());
			return Sky->GetRenderProxy();
		};
		auto Cube = LoadSky("/Engine/Materials/DefaultCubeSky.DefaultCubeSky", GDefaultMaterialService.CubeSky);
		if (Cube && IsRenderingThreadRunning())
			RefreshDefaultCubeSkyBindings_GameThread(*GDefaultMaterialService.CubeSky, GDefaultMaterialService.CubeSky->GetAcceptedCompiledProgram().get());
		auto Procedural = LoadSky("/Engine/Materials/DefaultProceduralSky.DefaultProceduralSky", GDefaultMaterialService.ProceduralSky);
		PublishDefaultMaterialProxy(GDefaultMaterialService.Proxy, std::move(Cube), std::move(Procedural));
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
		if (GDefaultMaterialService.ProceduralSky)
		{
			RemoveFromRoot(GDefaultMaterialService.ProceduralSky);
			GDefaultMaterialService.ProceduralSky = nullptr;
		}
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

	auto GetDefaultSkyMaterialProxy_RenderThread(bool bProcedural) -> FMaterialRenderProxyRef
	{
		CheckRenderingThread();
		return bProcedural ? GProceduralSkyProxy_RenderThread : GCubeSkyProxy_RenderThread;
	}

	auto GetDefaultMaterialRenderProxy_RenderThread() -> FMaterialRenderProxy*
	{
		CheckRenderingThread();
		return GDefaultMaterialProxy_RenderThread.GetReference();
	}
	auto GetDefaultCubeSkyMaterial() -> DMaterialInterface*
	{
		CheckDefaultMaterialGameThread();
		return GDefaultMaterialService.bAcceptingBindings ? GDefaultMaterialService.CubeSky : nullptr;
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
