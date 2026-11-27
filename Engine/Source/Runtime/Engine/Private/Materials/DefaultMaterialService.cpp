#include "Materials/DefaultMaterialService.h"
#include "Misc/UnifiedError.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Asset.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "SceneView.h"
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
		FSkyMaterialRequest GProceduralSky_RenderThread;
		uint64 GMaterialGeneration_RenderThread = 0;
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
					GProceduralSky_RenderThread = {};
				});
		}

		auto PrepareProceduralSkyMaterial_GameThread(uint64 Generation) -> void
		{
			if (!GDefaultMaterialService.bAcceptingBindings || Generation != GDefaultMaterialService.Generation) return;
			auto*& Material = GDefaultMaterialService.ProceduralSky;
			Material = LoadSkyMaterial(true);
			FMaterialRenderProxyRef Proxy;
			if (Material)
			{
				AddToRoot(Material);
				Material->CacheResourceShadersForRendering(EMaterialShaderPrecompileMode::Synchronous);
				if (Material->GetAcceptedCompiledProgram()) Proxy = Material->GetRenderProxy();
			}
			if (!Proxy)
				DURIN_ERROR_CATEGORY("Material", "Unable to prepare procedural skydome material: {}",
					Material ? Material->GetMaterialCookDiagnostic() : "asset unavailable");
			EnqueueRenderCommand("PublishSkyMaterial", [Generation, Proxy = std::move(Proxy)](FRHICommandListImmediate&) {
				if (Generation != GMaterialGeneration_RenderThread) return;
				auto& Slot = GProceduralSky_RenderThread;
				Slot.Proxy = Proxy;
				Slot.bPublished = true;
			});
		}
	}

	auto FCubeSkyCaptureMaterial::Create(const FViewEnvironmentOverride& Input)
		-> std::expected<std::unique_ptr<FCubeSkyCaptureMaterial>, std::string>
	{
		CheckDefaultMaterialGameThread();
		if (!GDefaultMaterialService.bAcceptingBindings || !Input.TextureReference)
			return std::unexpected("Cube sky material or texture reference is unavailable.");
		auto*& Parent = GDefaultMaterialService.CubeSky;
		if (!Parent)
		{
			Parent = LoadSkyMaterial(false);
			if (!Parent) return std::unexpected("DefaultCubeSky asset is unavailable.");
			AddToRoot(Parent);
		}
		if (!Parent->GetAcceptedCompiledProgram())
			Parent->CacheResourceShadersForRendering(EMaterialShaderPrecompileMode::Synchronous);
		if (!Parent->GetAcceptedCompiledProgram())
			return std::unexpected("DefaultCubeSky program is unavailable: " + Parent->GetMaterialCookDiagnostic());
		auto Capture = std::unique_ptr<FCubeSkyCaptureMaterial>(new FCubeSkyCaptureMaterial);
		Capture->Instance = DMaterialInstance::CreateDynamic(Parent);
		if (!Capture->Instance) return std::unexpected("Cannot create a DefaultCubeSky material instance.");
		const std::array Updates{
			FMaterialDynamicParameterUpdate::SetTextureReferenceByInfo({.Name = FName("SkyTexture")}, Input.TextureReference),
			FMaterialDynamicParameterUpdate::SetByInfo({.Name = FName("SkyTint")}, FMaterialParameterValue::MakeVector4(FVector4(Input.Tint, 1.0))),
			FMaterialDynamicParameterUpdate::SetByInfo({.Name = FName("SkyIntensity")}, FMaterialParameterValue::MakeScalar(std::max(Input.Intensity, 0.0f)))};
		if (auto Result = Capture->Instance->ApplyDynamicParameterUpdates(Updates); !Result)
			return std::unexpected(Result.error());
		auto Proxy = Capture->Instance->GetRenderProxy();
		if (!Proxy) return std::unexpected("DefaultCubeSky material instance has no render proxy.");
		Capture->Environment = std::make_shared<FCubeSkyViewEnvironment>(std::move(Proxy), Input.TextureReference, Input.Rotation);
		return Capture;
	}

	FCubeSkyCaptureMaterial::~FCubeSkyCaptureMaterial()
	{
		if (Instance) Instance->ClearParameterValueByInfo({.Name = FName("SkyTexture")});
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

	auto RequestProceduralSkyMaterialProxy_RenderThread() -> FMaterialRenderProxyRef
	{
		CheckRenderingThread();
		if (!GMaterialGeneration_RenderThread) return {};
		auto& Slot = GProceduralSky_RenderThread;
		if (Slot.bPublished) return Slot.Proxy;
		if (!Slot.Load.IsValid())
			Slot.Load = Tasks::LaunchTask("PrepareSkyMaterial", [Generation = GMaterialGeneration_RenderThread] {
				PrepareProceduralSkyMaterial_GameThread(Generation);
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
