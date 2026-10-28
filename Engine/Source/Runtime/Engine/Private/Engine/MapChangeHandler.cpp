#include "Engine/MapChangeHandler.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Asset/AsyncLoad.h"
#include "Engine/Level.h"
#include "Engine/Actor.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialProgramPreparation.h"
#include "Materials/DefaultMaterialService.h"
#include "Texture/Texture2D.h"
#include "IRendererModule.h"
#include "Rendering/SceneInterface.h"

namespace Durin
{
	auto FMapChangeHandler::DescribeException() -> std::string
	{
		try { throw; }
		catch (const std::exception& Error) { return Error.what(); }
		catch (...) { return "Unknown native exception."; }
	}

	auto FMapChangeHandler::SetPhase(EMapChangePhase Next) -> void
	{
		if (Phase == Next) return;
		const auto Now = std::chrono::steady_clock::now();
		DURIN_DEBUG("Map change {}: {} -> {} (phase elapsed {:.2f} ms)", Id,
			GetMapChangePhaseLabel(Phase), GetMapChangePhaseLabel(Next),
			std::chrono::duration<double, std::milli>(Now - PhaseStarted).count());
		Phase = Next; PhaseStarted = Now;
	}
	auto FMapChangeHandler::RetireCandidate() -> void
	{
		if (World) { auto* Retired = World.Get(); Retired->Shutdown(); World.Reset(); MarkObjectHierarchyAsGarbage(Retired); }
		if (Scene) { Scene->Release(); Scene.reset(); }
		if (Assets) Assets->Cancel();
		Assets.reset();
	}
	auto FMapChangeHandler::Fail(std::string Cause) -> void
	{
		// A callback may cancel and then return/throw an error while unwinding.
		// The Tick guard will finish that cancellation exactly once.
		if (bCancelRequested || IsMapChangeComplete(Phase)) return;
		Error = std::format("Map change {} / {}: {}", Id, GetMapChangePhaseLabel(Phase), Cause);
		SetPhase(EMapChangePhase::Failed);
		RetireCandidate();
	}
	auto FMapChangeHandler::PrepareProducts(DLevel& Level) -> std::expected<bool, std::string>
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
		uint32 Requests = 4;
		for (const auto& Owner : Meshes)
		{
			auto* Mesh = Cast<DStaticMesh>(Owner.Get());
			if (!IsValid(Mesh)) return std::unexpected("required mesh was unloaded.");
			const auto Ready = Mesh->PrepareRenderResources(Requests);
			if (!Ready) return std::unexpected(Ready.error());
			Pending |= !*Ready;
		}
		std::string ProgramError;
		std::erase_if(Programs, [&](auto& Program) {
			const auto State = Program.Poll();
			if (State == EMaterialProgramPreparationState::Pending) return false;
			if (State != EMaterialProgramPreparationState::Ready) ProgramError = "required material program failed: " + Program.GetDiagnostic();
			return true;
		});
		if (!ProgramError.empty()) return std::unexpected(std::move(ProgramError));
		while (Programs.size() < 8 && NextMaterial < Materials.size())
		{
			auto* Material = Cast<DMaterialInterface>(Materials[NextMaterial++].Get());
			if (!IsValid(Material)) return std::unexpected("required material was unloaded.");
			if (Material->NeedsCookedProgramPreparation())
				Programs.push_back(FMaterialProgramPreparation::Request(*Material));
		}
		if (!Programs.empty() || NextMaterial != Materials.size()) return false;
		std::unordered_set<DTexture2D*> Textures;
		std::vector<DTexture2D*> UsedTextures;
		for (const auto& Owner : Materials)
		{
			auto* Material = Cast<DMaterialInterface>(Owner.Get());
			if (!IsValid(Material)) return std::unexpected("required material was unloaded.");
			const auto Ready = Material->GetProgramReadiness();
			if (!Ready) return std::unexpected(Ready.error());
			Pending |= !*Ready;
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
		uint32 TextureRequests = 4;
		for (auto* Texture : Textures)
		{
			const auto Ready = Texture->PrepareRenderResource(TextureRequests);
			if (!Ready) return std::unexpected(Ready.error());
			Pending |= !*Ready;
		}
		return !Pending;
	}

	FMapChangeHandler::FMapChangeHandler(DEngine& InHost, FMapChangeRequest Request)
		: Host(InHost), Path(std::move(Request.Path)), GameMode(Request.GameMode),
		  BeforeCommit(std::move(Request.BeforeCommit)),
		  OnComplete(std::move(Request.OnComplete))
	{
		CheckGameThread();
		static uint64 NextId = 0;
		Id = ++NextId;
		DURIN_DEBUG("Map change {}: loading {}", Id, Path.ToString());
	}
	FMapChangeHandler::~FMapChangeHandler() { Cancel(); }
	auto FMapChangeHandler::GetProgress() const -> FMapChangeProgress
	{
		CheckGameThread();
		const auto Now = std::chrono::steady_clock::now();
		return {Id, Phase, std::chrono::duration<double>(Now - Started).count(),
			std::chrono::duration<double>(Now - PhaseStarted).count(), bCollected,
			Meshes.size(), Materials.size(), Programs.size(), Error};
	}
	auto FMapChangeHandler::GetLevel() const -> DLevel* { return Assets ? Cast<DLevel>(Assets->GetLoadedObject()) : nullptr; }
	auto FMapChangeHandler::Cancel() -> void
	{
		CheckGameThread();
		if (IsMapChangeComplete(Phase)) return;
		if (bAdvancing)
		{
			bCancelRequested = true;
			if (World) World->RequestShutdown();
			return;
		}
		Error = "Map change canceled.";
		SetPhase(EMapChangePhase::Canceled);
		RetireCandidate();
	}
	auto FMapChangeHandler::Tick() -> void
	{
		CheckGameThread();
		if (bAdvancing) return;
		bAdvancing = true;
		struct FTickScope
		{
			FMapChangeHandler& Loading;
			const EMapChangePhase Phase = Loading.Phase;
			const std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
			~FTickScope()
			{
				Loading.bAdvancing = false;
				const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
				if (Milliseconds >= 16.0)
					DURIN_DEBUG("Map change timing: {} / {} / {:.2f} ms (GameThread).", Loading.Id, GetMapChangePhaseLabel(Phase), Milliseconds);
				if (Loading.bCancelRequested) Loading.Cancel();
			}
		} Scope{*this};
		try
		{
			Advance();
			if (Phase == EMapChangePhase::Ready && bCommitRequested && BeforeCommit)
			{
				auto PrepareCommit = std::move(BeforeCommit);
				if (!PrepareCommit(*GetLevel())) Fail("Map activation was rejected by the caller.");
			}
		}
		catch (...) { Fail(DescribeException()); }
		// Publishing the World is irreversible. Do not turn a publication fault
		// into a recoverable preparation failure and tear down an adopted World.
		if (Phase == EMapChangePhase::Ready && bCommitRequested)
		{
			if (bCancelRequested || World->IsShutdownRequested()) Cancel();
			else Commit();
		}
	}

	auto FMapChangeHandler::Advance() -> void
	{
		switch (Phase)
		{
		case EMapChangePhase::LoadingAssets:
			if (!Assets) Assets = RequestAsyncLoad(Path, {}, DLevel::StaticClass());
			if (!Assets->IsComplete()) return;
			if (!Assets->GetResult()) return Fail(Assets->GetResult().error().Message);
			if (!GetLevel()) return Fail("required Level is missing.");
			SetPhase(EMapChangePhase::PreparingProducts);
			return;
		case EMapChangePhase::PreparingProducts:
		{
			const auto Products = PrepareProducts(*GetLevel());
			if (!Products) return Fail(Products.error());
			if (*Products) SetPhase(World ? EMapChangePhase::Ready : EMapChangePhase::RegisteringWorld);
			return;
		}
		case EMapChangePhase::RegisteringWorld:
			if (!Host.GetRendererModule()) return Fail("renderer services are unavailable.");
			Scene = Host.GetRendererModule()->CreateScene();
			if (!Scene) return Fail("candidate scene creation failed.");
			if (bCancelRequested) return;
			World = NewObject<DWorld>(&Host, FName(std::format("SceneCandidate{}", Id)));
			World->SetWorldType(Host.GetInitialWorldType());
			World->SetRenderScene(Scene.get());
			if (const auto Services = World->InitializeSubsystems(&Host); !Services) return Fail(Services.Message);
			if (bCancelRequested) return;
			if (!World->SetCurrentLevel(GetLevel())) return Fail("Level registration was rejected; the previous document remains active.");
			if (bCancelRequested) return;
			if (GameMode.has_value())
			{
				const auto Play = World->BeginPlay({.GameModeClass = *GameMode});
				if (!Play) return Fail(Play.Message);
				if (bCancelRequested) return;
				// BeginPlay may spawn required geometry/materials. Admit the final
				// stopped-in-time scene before enabling input or simulation.
				Meshes.clear(); Materials.clear(); MaterialBindings.clear();
				NextMaterial = 0; bCollected = false;
				SetPhase(EMapChangePhase::PreparingProducts);
				return;
			}
			SetPhase(EMapChangePhase::Ready);
			return;
		default: return;
		}
	}
	auto FMapChangeHandler::Commit() -> void
	{
		World->LevelResidency = std::move(Assets);
		Host.AdoptSceneWorld(World.Get(), std::move(Scene), Id);
		World.Reset();
		SetPhase(EMapChangePhase::Committed);
	}
}
