#include "Engine/SceneLoadOperation.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Asset/AsyncLoad.h"
#include "Client/SceneViewport.h"
#include "Engine/Level.h"
#include "Engine/Actor.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Asset/CookedMeshLoadManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialProgramPreparation.h"
#include "Materials/DefaultMaterialService.h"
#include "Texture/Texture2D.h"
#include "IRendererModule.h"
#include "Rendering/SceneInterface.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	struct FSceneLoadOperation::FImpl
	{
		DEngine& Host;
		EWorldType Type;
		uint64 Id;
		ESceneLoadPhase Phase = ESceneLoadPhase::LoadingAssets;
		std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now(), PhaseStarted = Started;
		std::string Error;
		std::shared_ptr<FAsyncLoadHandle> Assets;
		TStrongObjectPtr<DWorld> World;
		FScenePtr Scene;
		std::vector<FWeakObjectPtr> Meshes, Materials, MaterialBindings;
		std::vector<FMaterialProgramPreparation> Programs;
		size_t NextMaterial = 0;
		bool bCollected = false;
		bool bAdvancing = false, bCancelRequested = false;
		bool bPlayRequested = false, bWorldRegistered = false;
		bool bComponentsReady = false;
		DClass* GameMode = nullptr;
		std::function<std::expected<FSceneView, std::string>(const DWorld&)> BuildInitialView;
		std::optional<FSceneView> InitialView;
		FTextureRHIRef Output;
		FRenderResourcePreparation Preparation;
		struct FViewResult { std::atomic<bool> InFlight = false; std::atomic<ERenderViewResult> Result = ERenderViewResult::RendererResourcesPending; };
		std::shared_ptr<FViewResult> ViewResult = std::make_shared<FViewResult>();
		auto SetPhase(ESceneLoadPhase Next) -> void
		{
			if (Phase == Next) return;
			const auto Now = std::chrono::steady_clock::now();
			DURIN_DEBUG("Scene operation {}: {} -> {} (phase elapsed {:.2f} ms)", Id,
				GetSceneLoadPhaseLabel(Phase), GetSceneLoadPhaseLabel(Next),
				std::chrono::duration<double, std::milli>(Now - PhaseStarted).count());
			Phase = Next; PhaseStarted = Now;
		}
		auto RetireCandidate() -> void
		{
			if (World) { auto* Retired = World.Get(); Retired->Shutdown(); World.Reset(); MarkObjectHierarchyAsGarbage(Retired); }
			if (Scene) { Scene->Release(); Scene.reset(); }
			Preparation.RequestCancel();
			Output = nullptr;
		}
		auto Fail(std::string Cause) -> void
		{
			Error = std::format("Scene operation {}: {}", Id, Cause);
			SetPhase(ESceneLoadPhase::Failed);
			RetireCandidate();
			Assets.reset();
		}
		auto PrepareProducts(DLevel& Level) -> std::expected<bool, std::string>
		{
			if (!bCollected)
			{
				std::unordered_set<DStaticMesh*> SeenMeshes;
				std::unordered_set<DMaterialInterface*> SeenMaterials;
				std::unordered_set<DMaterialInterface*> SeenBindings;
				for (const auto& Actor : Level.GetActors())
					if (IsValid(Actor.Get())) for (auto* Component : Actor->FindComponentsByClass<DMeshComponent>())
					{
						if (!Component->IsVisible() || Actor->IsHidden()) continue;
						DStaticMesh* Mesh = nullptr;
						if (auto* Static = Cast<DStaticMeshComponent>(Component)) Mesh = Static->GetStaticMesh();
						if (auto* Spline = Cast<DSplineMeshComponent>(Component)) Mesh = Spline->GetStaticMesh();
						if (IsValid(Mesh) && SeenMeshes.insert(Mesh).second) Meshes.emplace_back(Mesh);
						for (uint32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
						{
							auto* EffectiveMaterial = Component->GetMaterial(Slot);
							if (!EffectiveMaterial) EffectiveMaterial = GetDefaultMaterial();
							if (IsValid(EffectiveMaterial) && SeenBindings.insert(EffectiveMaterial).second) MaterialBindings.emplace_back(EffectiveMaterial);
							for (auto* Material = EffectiveMaterial; IsValid(Material) && SeenMaterials.insert(Material).second; Material = Material->GetParent())
								Materials.emplace_back(Material);
						}
					}
				bCollected = true;
				if (Meshes.size() > 4096 || Materials.size() > 4096) return std::unexpected("required product owners exceed 4096.");
			}
			bool Pending = false;
			uint32 Requests = 0;
			for (const auto& Owner : Meshes)
			{
				auto* Mesh = Cast<DStaticMesh>(Owner.Get());
				if (!IsValid(Mesh)) return std::unexpected("required mesh was unloaded.");
				if (Mesh->IsCompiling()) { Pending = true; continue; }
				const auto Diagnostic = Mesh->GetCompilationDiagnostic();
				if (Diagnostic.Status && *Diagnostic.Status != EStaticMeshCompilationStatus::Succeeded)
					return std::unexpected(FormatStaticMeshCompilationDiagnostic(Diagnostic));
				const auto Status = Mesh->GetRenderDataLoadStatus();
				if (Status.CpuPhase == ECookedMeshCpuPhase::Failed || Status.CpuPhase == ECookedMeshCpuPhase::Cancelled || Status.GpuPhase == ECookedMeshGpuPhase::Failed)
					return std::unexpected("required mesh render data failed.");
				if (Status.CpuPhase == ECookedMeshCpuPhase::CpuReady && Status.GpuPhase == ECookedMeshGpuPhase::Ready) continue;
				Pending = true;
				if (Requests++ < 4)
				{
					if (const auto* Manager = GetCookedMeshLoadManager())
					{
						const auto Work = Manager->GetDiagnostics();
						if (Work.InFlightCount + Work.PendingRequestCount >= 8) break;
					}
					(void)Mesh->RequestRenderDataAndResources();
				}
			}
			bool Failed = false;
			std::erase_if(Programs, [&](auto& Program) {
				const auto State = Program.Poll();
				if (State == EMaterialProgramPreparationState::Pending) return false;
				if (State != EMaterialProgramPreparationState::Ready) { Failed = true; Error = Program.GetDiagnostic(); }
				return true;
			});
			if (Failed) return std::unexpected("required material program failed: " + Error);
			while (Programs.size() < 8 && NextMaterial < Materials.size())
			{
				auto* Material = Cast<DMaterialInterface>(Materials[NextMaterial++].Get());
				if (!IsValid(Material)) return std::unexpected("required material was unloaded.");
				if (GetAssetRuntimeConfiguration().RequiresCookedPayload() && !Material->IsDynamicInstance() && !Material->IsComplete())
					Programs.push_back(FMaterialProgramPreparation::Request(*Material));
			}
			if (!Programs.empty() || NextMaterial != Materials.size()) return false;
			std::unordered_set<DTexture2D*> Textures;
			std::vector<DTexture2D*> UsedTextures;
			for (const auto& Owner : Materials)
			{
				auto* Material = Cast<DMaterialInterface>(Owner.Get());
				if (!IsValid(Material)) return std::unexpected("required material was unloaded.");
				if (Material->IsCompiling()) { Pending = true; continue; }
				if (!Material->IsComplete())
					return std::unexpected(std::format("required material '{}' has no complete program (state {}): {}",
						Material->GetObjectPath(), static_cast<uint32>(Material->GetMaterialCompileStatus().State), Material->GetMaterialCookDiagnostic()));
			}
			if (Pending) return false;
			for (const auto& Owner : MaterialBindings)
			{
				auto* Material = Cast<DMaterialInterface>(Owner.Get());
				if (!IsValid(Material)) return std::unexpected("required material binding was unloaded.");
				Material->GetUsedTextures(UsedTextures);
				Textures.insert(UsedTextures.begin(), UsedTextures.end());
				if (Textures.size() > 4096) return std::unexpected("required textures exceed 4096.");
			}
			uint32 TextureRequests = 0;
			for (auto* Texture : Textures)
			{
				if (Texture->HasUsableResource()) continue;
				if (!Texture->IsAsyncCacheComplete() || Texture->IsResourceUpdatePending()) { Pending = true; continue; }
				const auto State = Texture->GetResourceUpdateState();
				if (State == ETextureResourceUpdateState::Failed || State == ETextureResourceUpdateState::Closed)
					return std::unexpected(std::format("required texture '{}': {}", Texture->GetObjectPath(), Texture->GetResourceUpdateError()));
				if (++TextureRequests > 4) { Pending = true; continue; }
				if (!Texture->HasPlatformData())
				{
					if (!GetAssetRuntimeConfiguration().RequiresCookedPayload() || !Texture->EnsurePlatformDataLoadedBlocking())
						return std::unexpected(std::format("required texture '{}' has no platform data.", Texture->GetObjectPath()));
				}
				else Texture->UpdateResource();
				Pending = true;
			}
			return !Pending;
		}

		auto PrepareComponents() -> std::expected<bool, std::string>
		{
			DPrimitiveComponent::ProcessPSOPrecacheRequests(World.Get());
			bool Ready = true;
			for (const auto& Actor : World->GetCurrentLevel()->GetActors())
				if (IsValid(Actor.Get()) && !Actor->IsHidden())
					for (auto* Component : Actor->FindComponentsByClass<DMeshComponent>())
					{
						if (!Component->IsRegistered() || !Component->IsVisible()) continue;
						if (Component->IsPSOPrecacheReadyForProxyCreation()) continue;
						for (const auto& Request : Component->GetPSOPrecacheComponentData().GetMaterialPSOPrecacheRequestIDs())
						{
							const auto Progress = Request.GetProgress();
							if (Request.IsCurrent() && Progress.bRequiredReady) continue;
							if (Progress.State == EMaterialPSOPrecacheState::Failed || Progress.State == EMaterialPSOPrecacheState::Canceled)
								return std::unexpected(std::format("required component '{}': {}", Component->GetObjectPath(), Request.GetDiagnostic()));
						}
						Ready = false;
					}
			return Ready;
		}
	};

	FSceneLoadOperation::FSceneLoadOperation(DEngine& Host, FSceneLoadRequest Request, bool bAdmitted)
	{
		CheckGameThread();
		static uint64 NextId = 0;
		Impl = std::make_unique<FImpl>(Host, Host.GetInitialWorldType(), ++NextId);
		if (!bAdmitted) { Impl->Fail("Scene admission is closed."); return; }
		Impl->bPlayRequested = Request.GameMode.has_value();
		Impl->GameMode = Request.GameMode.value_or(nullptr);
		Impl->BuildInitialView = std::move(Request.BuildInitialView);
		Impl->Assets = RequestAsyncLoad(Request.Path, {}, DLevel::StaticClass());
	}
	FSceneLoadOperation::~FSceneLoadOperation() { Cancel(); }
	auto FSceneLoadOperation::GetProgress() const -> FSceneLoadProgress
	{
		CheckGameThread();
		const auto Now = std::chrono::steady_clock::now();
		return {Impl->Id, Impl->Phase, std::chrono::duration<double>(Now - Impl->Started).count(),
			std::chrono::duration<double>(Now - Impl->PhaseStarted).count(), Impl->bCollected,
			Impl->Meshes.size(), Impl->Materials.size(), Impl->Programs.size(), Impl->Error};
	}
	auto FSceneLoadOperation::GetLevel() const -> DLevel* { return Impl->Assets ? Cast<DLevel>(Impl->Assets->GetLoadedObject()) : nullptr; }
	auto FSceneLoadOperation::Cancel() -> void
	{
		CheckGameThread();
		if (Impl->Phase == ESceneLoadPhase::Committed || Impl->Phase == ESceneLoadPhase::Canceled || Impl->Phase == ESceneLoadPhase::Failed) return;
		if (Impl->bAdvancing)
		{
			Impl->bCancelRequested = true;
			if (Impl->World) Impl->World->RequestShutdown();
			return;
		}
		Impl->SetPhase(ESceneLoadPhase::Canceled);
		if (Impl->Assets) Impl->Assets->Cancel();
		Impl->RetireCandidate();
		Impl->Assets.reset();
	}
	auto FSceneLoadOperation::Advance() -> void
	{
		CheckGameThread();
		auto& Loading = *Impl;
		if (Loading.bAdvancing) return;
		Loading.bAdvancing = true;
		struct FAdvanceScope
		{
			FSceneLoadOperation& Owner;
			FImpl& Loading;
			const ESceneLoadPhase Phase = Loading.Phase;
			const std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
			~FAdvanceScope()
			{
				Loading.bAdvancing = false;
				const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
				if (Milliseconds >= 16.0)
					DURIN_DEBUG("Scene timing: operation {} / {} / {:.2f} ms (GameThread).", Loading.Id, GetSceneLoadPhaseLabel(Phase), Milliseconds);
				if (Loading.bCancelRequested) Owner.Cancel();
			}
		} Scope{*this, Loading};
		try
		{
		switch (Loading.Phase)
		{
		case ESceneLoadPhase::LoadingAssets:
			if (!Loading.Assets->IsComplete()) return;
			if (!Loading.Assets->GetResult()) return Loading.Fail(Loading.Assets->GetResult().error().Message);
			if (!GetLevel()) return Loading.Fail("required Level is missing.");
			Loading.SetPhase(ESceneLoadPhase::PreparingProducts);
			return;
		case ESceneLoadPhase::PreparingProducts:
		{
			const auto Products = Loading.PrepareProducts(*GetLevel());
			if (!Products) return Loading.Fail(Products.error());
			if (*Products) Loading.SetPhase(Loading.bWorldRegistered ? ESceneLoadPhase::PreparingView : ESceneLoadPhase::RegisteringWorld);
			return;
		}
		case ESceneLoadPhase::RegisteringWorld:
			if (!Loading.Host.GetRendererModule()) return Loading.Fail("renderer services are unavailable.");
			Loading.Scene = Loading.Host.GetRendererModule()->CreateScene();
			if (!Loading.Scene) return Loading.Fail("candidate scene creation failed.");
			if (Loading.bCancelRequested) return;
			Loading.World = NewObject<DWorld>(&Loading.Host, FName(std::format("SceneCandidate{}", Loading.Id)));
			Loading.World->SetWorldType(Loading.Type);
			Loading.World->SetRenderScene(Loading.Scene.get());
			if (const auto Services = Loading.World->InitializeSubsystems(&Loading.Host); !Services) return Loading.Fail(Services.Message);
			if (Loading.bCancelRequested) return;
			if (!Loading.World->SetCurrentLevel(GetLevel())) return Loading.Fail("Level registration was rejected; the previous document remains active.");
			if (Loading.bCancelRequested) return;
			Loading.Preparation = FRenderResourcePreparation::Create();
			Loading.bWorldRegistered = true;
			if (Loading.bPlayRequested)
			{
				const auto Play = Loading.World->BeginPlay({.GameModeClass = Loading.GameMode});
				if (!Play) return Loading.Fail(Play.Message);
				if (Loading.bCancelRequested) return;
				// BeginPlay may spawn required geometry/materials. Admit the final
				// stopped-in-time scene before enabling input or simulation.
				Loading.Meshes.clear(); Loading.Materials.clear(); Loading.MaterialBindings.clear();
				Loading.NextMaterial = 0; Loading.bCollected = false;
				Loading.SetPhase(ESceneLoadPhase::PreparingProducts);
				return;
			}
			Loading.SetPhase(ESceneLoadPhase::PreparingView);
			return;
		case ESceneLoadPhase::PreparingView:
		{
			const auto Components = Loading.PrepareComponents();
			if (!Components) return Loading.Fail(Components.error());
			if (!*Components)
			{
				Loading.bComponentsReady = false;
				return;
			}
			if (!Loading.bComponentsReady)
			{
				// A view completed before component invalidation cannot authorize the
				// replacement set. Retire its receipt after its command has finished.
				if (Loading.ViewResult->InFlight.load(std::memory_order_acquire)) return;
				Loading.ViewResult->Result = ERenderViewResult::RendererResourcesPending;
				Loading.bComponentsReady = true;
			}
			if (!Loading.InitialView)
			{
				if (Loading.BuildInitialView)
				{
					auto View = Loading.BuildInitialView(*Loading.World);
					if (!View) return Loading.Fail(View.error());
					Loading.InitialView = std::move(*View);
					Loading.BuildInitialView = {};
				}
				else
				{
					const auto& Viewport = Loading.Host.GetMainSceneViewport();
					if (!Viewport) return Loading.Fail("initial viewport is unavailable.");
					const auto Size = Viewport->GetDesiredSize();
					Loading.InitialView = Loading.Host.BuildWorldSceneView(*Loading.World,
						static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(Size.x))),
						static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(Size.y))));
				}
				if (!Loading.InitialView->ViewportWidth || !Loading.InitialView->ViewportHeight)
					return Loading.Fail("initial view has no output extent.");
				Loading.InitialView->ViewStateId = {};
				Loading.InitialView->bDiscardHistory = true;
			}
			if (Loading.ViewResult->InFlight.load(std::memory_order_acquire)) return;
			if (Loading.ViewResult->Result == ERenderViewResult::Success) Loading.SetPhase(ESceneLoadPhase::Ready);
			else if (Loading.ViewResult->Result != ERenderViewResult::RendererResourcesPending) Loading.Fail("required initial view resources failed.");
			return;
		}
		default: return;
		}
		}
		catch (const std::exception& Error) { Loading.Fail(Error.what()); }
		catch (...) { Loading.Fail("native scene preparation threw an exception."); }
	}
	auto FSceneLoadOperation::SubmitView() -> void
	{
		CheckGameThread();
		auto& Loading = *Impl;
		if (Loading.Phase != ESceneLoadPhase::PreparingView || !Loading.bComponentsReady || !Loading.InitialView || Loading.ViewResult->InFlight.exchange(true)) return;
		try
		{
		if (!Loading.Output)
			Loading.Output = RHICreateTexture(FRHITextureCreateDesc::Create2D("CandidateScene", Loading.InitialView->ViewportWidth,
				Loading.InitialView->ViewportHeight, EPixelFormat::SRGBA8_UNORM).SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource));
		ENQUEUE_RENDER_COMMAND(PrepareCandidateScene)([Scene = Loading.Scene.get(), Renderer = Loading.Host.GetRendererModule(),
			View = *Loading.InitialView, Output = Loading.Output, Preparation = Loading.Preparation, Receipt = Loading.ViewResult](FRHICommandListImmediate& Commands) {
			Receipt->Result = Renderer->RenderView(Commands, {Scene, View, Output, false, {}, nullptr, nullptr, &Preparation});
			Receipt->InFlight.store(false, std::memory_order_release);
		});
		}
		catch (const std::exception& Error) { Loading.Fail(Error.what()); }
		catch (...) { Loading.Fail("initial view submission threw an exception."); }
	}
	auto FSceneLoadOperation::Commit() -> bool
	{
		CheckGameThread();
		if (Impl->Phase != ESceneLoadPhase::Ready || !Impl->World || Impl->World->IsShutdownRequested()) return false;
		if (Impl->bAdvancing) return false;
		Impl->bAdvancing = true;
		struct FCommitScope
		{
			FSceneLoadOperation& Owner;
			FImpl& Loading;
			~FCommitScope() { Loading.bAdvancing = false; if (Loading.bCancelRequested) Owner.Cancel(); }
		} Scope{*this, *Impl};
		Impl->Host.AdoptSceneWorld(Impl->World.Get(), std::move(Impl->Scene), Impl->Id);
		Impl->World.Reset();
		Impl->Output = nullptr;
		Impl->SetPhase(ESceneLoadPhase::Committed);
		return true;
	}
}
