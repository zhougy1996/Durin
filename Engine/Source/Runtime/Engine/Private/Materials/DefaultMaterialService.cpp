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
#include "Threading/Task.h"
#include "RenderPipelineCreation.h"

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
			uint64 Generation = 0;
			bool bInitialized = false;
			bool bAcceptingBindings = false;
		};

		FDefaultMaterialServiceState GDefaultMaterialService;
		FMaterialRenderProxyRef GDefaultMaterialProxy_RenderThread;
		struct FSkyMaterialRequest
		{
			FMaterialRenderProxyRef Proxy;
			Tasks::TTask<void> Load;
			bool bPublished = false;
		};
		FSkyMaterialRequest GCubeSky_RenderThread;
		FSkyMaterialRequest GProceduralSky_RenderThread;
		uint64 GMaterialGeneration_RenderThread = 0;
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

		auto LoadSkyMaterial(bool bProcedural) -> DMaterial*
		{
			const char* Name = bProcedural ? "/Engine/Materials/DefaultProceduralSky.DefaultProceduralSky"
				: "/Engine/Materials/DefaultCubeSky.DefaultCubeSky";
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Name, Path)) return nullptr;
			return LoadObject<DMaterial>(Path);
		}

		auto PublishDefaultMaterialProxy(FMaterialRenderProxyRef Proxy, uint64 Generation) -> void
		{
			if (IsRenderingThreadRunning())
				EnqueueRenderCommand("PublishDefaultMaterialProxy", [Proxy = std::move(Proxy), Generation](FRHICommandListImmediate&) {
					GDefaultMaterialProxy_RenderThread = Proxy;
					GMaterialGeneration_RenderThread = Generation;
					GCubeSky_RenderThread = {};
					GProceduralSky_RenderThread = {};
					GCubeSkyBindings_RenderThread.reset();
				});
		}

		auto PrepareSkyMaterial_GameThread(bool bProcedural, uint64 Generation) -> void
		{
			if (!GDefaultMaterialService.bAcceptingBindings || Generation != GDefaultMaterialService.Generation) return;
			auto*& Material = bProcedural ? GDefaultMaterialService.ProceduralSky : GDefaultMaterialService.CubeSky;
			Material = LoadSkyMaterial(bProcedural);
			FMaterialRenderProxyRef Proxy;
			if (Material)
			{
				AddToRoot(Material);
				Material->CacheResourceShadersForRendering(EMaterialShaderPrecompileMode::Synchronous);
				const auto Program = Material->GetAcceptedCompiledProgram();
				if (Program)
				{
					if (!bProcedural) RefreshDefaultCubeSkyBindings_GameThread(*Material, Program.get());
					Proxy = Material->GetRenderProxy();
				}
			}
			if (!Proxy)
				DURIN_ERROR_CATEGORY("Material", "Unable to prepare {} skydome material: {}", bProcedural ? "procedural" : "cubemap",
					Material ? Material->GetMaterialCookDiagnostic() : "asset unavailable");
			EnqueueRenderCommand("PublishSkyMaterial", [bProcedural, Generation, Proxy = std::move(Proxy)](FRHICommandListImmediate&) {
				if (Generation != GMaterialGeneration_RenderThread) return;
				auto& Slot = bProcedural ? GProceduralSky_RenderThread : GCubeSky_RenderThread;
				Slot.Proxy = Proxy;
				Slot.bPublished = true;
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
		PublishDefaultMaterialProxy(GDefaultMaterialService.Proxy, ++GDefaultMaterialService.Generation);
		GDefaultMaterialService.bAcceptingBindings = true;
		return true;
	}

	auto ShutdownDefaultMaterialService() -> void
	{
		CheckDefaultMaterialGameThread();
		if (!GDefaultMaterialService.bInitialized) return;
		GDefaultMaterialService.bAcceptingBindings = false;
		PublishDefaultMaterialProxy({}, 0);
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

	auto RequestSkyMaterialProxy_RenderThread(bool bProcedural) -> FMaterialRenderProxyRef
	{
		CheckRenderingThread();
		if (!GMaterialGeneration_RenderThread) return {};
		auto& Slot = bProcedural ? GProceduralSky_RenderThread : GCubeSky_RenderThread;
		if (Slot.bPublished) return Slot.Proxy;
		if (!Slot.Load.IsValid())
			Slot.Load = Tasks::LaunchTask("PrepareSkyMaterial", [bProcedural, Generation = GMaterialGeneration_RenderThread] {
				PrepareSkyMaterial_GameThread(bProcedural, Generation);
			}, Tasks::ETaskThread::GameThread);
		// Publication follows material and binding updates on the render command pipe.
		if (Slot.Load.IsValid() && (!Slot.Load.IsCompleted() || Slot.Load.GetState() == ETaskState::Succeeded))
			FRenderPipelinePreparationBatch::MarkResourcePending();
		return {};
	}

	auto GetDefaultMaterialRenderProxy_RenderThread() -> FMaterialRenderProxy*
	{
		CheckRenderingThread();
		return GDefaultMaterialProxy_RenderThread.GetReference();
	}
	auto GetDefaultCubeSkyMaterial() -> DMaterialInterface*
	{
		CheckDefaultMaterialGameThread();
		return GDefaultMaterialService.bAcceptingBindings ? LoadSkyMaterial(false) : nullptr;
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
