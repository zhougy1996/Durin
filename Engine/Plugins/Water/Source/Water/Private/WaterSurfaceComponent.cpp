#include "WaterSurfaceComponent.h"

#include "WaterSurfaceResources.h"
#include "Components/PropertyEditValidation.h"
#include "DObject/Property.h"
#include "Logging/LogMacros.h"
#include "Materials/MaterialInterface.h"
#include "Math/Operations.h"
#include "Rendering/PrimitiveCustomData.h"
#include "Rendering/SceneInterface.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		static_assert(FWaterWaveData{}.size() <= PrimitiveCustomDataFloatCount);

		auto ResolveWaterSettings(const FWaterSurfaceSettings& Settings, const DWaterWaves* Asset)
			-> std::expected<FWaterSurfaceSettings, std::string>
		{
			auto Effective = Settings;
			if (Asset)
			{
				const auto& Group = Asset->GetWaveGroup();
				if (!Group) return std::unexpected(Group.error());
				Effective.Amplitude = Group->BaseAmplitude;
				Effective.Wavelength = Group->BaseWavelength;
				Effective.Waves = Group->Waves;
			}
			if (auto Validation = ValidateWaterSurface(Effective); !Validation) return std::unexpected(Validation.error());
			return Effective;
		}

		class FWaterSceneProxy final : public FStaticMeshSceneProxy
		{
		public:
			FWaterSceneProxy(std::shared_ptr<FStaticMeshRenderData> Geometry, FMaterialRenderProxyRef Material, const FWaterInstanceWaveData& Data)
				: FStaticMeshSceneProxy(Geometry.get(), std::vector{std::move(Material)}), Owner(std::move(Geometry))
			{
				static_assert(FWaterInstanceWaveData{}.size() <= PrimitiveCustomDataFloatCount);
				verify(InstanceData.Set(0, Data));
			}
			auto GetDefaultInstanceCustomData() const -> FPrimitiveCustomData override { return InstanceData; }
		private:
			// The ordinary StaticMesh proxy borrows geometry. Retain it through removal
			// so edits and unregister never retire resources still used by a draw.
			std::shared_ptr<FStaticMeshRenderData> Owner;
			FPrimitiveCustomData InstanceData;
		};
	}

	DWaterSurfaceComponent::DWaterSurfaceComponent(const FObjectInitializer& Initializer) : Super(Initializer)
	{
		SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SetCastShadow(false);
		FSoftObjectPath Path;
		[[maybe_unused]] const bool bValidPath = FSoftObjectPath::TryCreate("/Plugins/Water/Materials/DefaultWater.DefaultWater", Path);
		check(bValidPath);
		DefaultMaterialAsset = std::move(Path);
	}

	auto DWaterSurfaceComponent::HasSupportedTransform() const -> bool
	{
		const auto& S = GetWorldTransform().Scale3D;
		return std::isfinite(S.x) && std::isfinite(S.y) && std::isfinite(S.z)
			&& S.x > 1.e-6 && std::abs(S.x - S.y) <= 1.e-8 * S.x && std::abs(S.x - S.z) <= 1.e-8 * S.x;
	}

	auto DWaterSurfaceComponent::SetSettings(const FWaterSurfaceSettings& Value) -> std::expected<void, std::string>
	{
		if (auto E = ValidateWaterSurface(Value); !E) return E;
		if (WaveAsset)
			if (const auto Effective = ResolveWaterSettings(Value, WaveAsset.Get()); !Effective)
				return std::unexpected(Effective.error());
		if (Settings == Value) return {};
		Settings = Value;
		MarkPackageDirty();
		RefreshSurface();
		return {};
	}

	auto DWaterSurfaceComponent::SetWaveAsset(DWaterWaves* Value) -> std::expected<void, std::string>
	{
		if (WaveAsset.Get() == Value) return {};
		if (const auto Effective = ResolveWaterSettings(Settings, Value); !Effective)
			return std::unexpected(Effective.error());
		WaveAsset = Value;
		MarkPackageDirty();
		RefreshSurface();
		return {};
	}

	auto DWaterSurfaceComponent::BindWaveAsset(DWaterWaves* Asset) -> void
	{
		if (BoundWaveAsset.Get() == Asset && (Asset || !WaveAssetChangeHandle.IsValid())) return;
		if (auto* Previous = BoundWaveAsset.Get()) Previous->OnWavesChanged().Remove(WaveAssetChangeHandle);
		WaveAssetChangeHandle = {};
		BoundWaveAsset = Asset;
		if (Asset)
		{
			WaveAssetChangeHandle = Asset->OnWavesChanged().AddLambda([Weak = TWeakObjectPtr<DWaterSurfaceComponent>(this)] {
				if (auto* Component = Weak.Get()) Component->RefreshSurface();
			});
		}
	}

	auto DWaterSurfaceComponent::RefreshSurface() -> void
	{
		if (!IsRegistered()) return;
		BindWaveAsset(WaveAsset.Get());
		// The replacement proxy publishes the new wave data with its grid and bounds.
		DestroyRenderState();
		SynchronizeSurface();
		MarkRenderStateDirty();
	}

	auto DWaterSurfaceComponent::SynchronizeSurface() -> void
	{
		auto Effective = ResolveWaterSettings(Settings, WaveAsset.Get());
		if (!Effective)
		{
			EffectiveSettings.reset();
			Geometry.reset();
			verify(SetCustomPrimitiveData(0, FWaterWaveData{}));
			Diagnostic = Effective.error();
			return;
		}
		Diagnostic.clear();
		EffectiveSettings = std::move(*Effective);
		verify(SetCustomPrimitiveData(0, PackWaterWaveData(*EffectiveSettings)));
		if (!GetRenderScene()) { Geometry.reset(); return; }
		auto Data = MakeWaterGrid(*EffectiveSettings);
		Geometry = std::shared_ptr<FStaticMeshRenderData>(Data.release(), [](FStaticMeshRenderData* P) {
			EnqueueRenderCommand("ReleaseWaterGeometry", [Owner = std::shared_ptr<FStaticMeshRenderData>(P)](FRHICommandListImmediate&) {
				Owner->ReleaseResources();
			});
		});
		EnqueueRenderCommand("InitializeWaterGeometry", [Owner = Geometry](FRHICommandListImmediate& Commands) {
			if (!Owner->InitResources(Commands)) DURIN_ERROR("Water grid render-resource initialization failed.");
		});
	}

	auto DWaterSurfaceComponent::SetWaveGenerator(const IWaterWaveGenerator& Generator) -> std::expected<void, std::string>
	{
		if (WaveAsset) return std::unexpected("Clear the Water Waves Asset before applying a local wave generator.");
		auto Candidate = Settings;
		Candidate.Waves = Generator.GenerateWaves();
		return SetSettings(Candidate);
	}

	auto DWaterSurfaceComponent::ResolveMaterial() -> FMaterialRenderProxyRef
	{
		const auto Feature = GetRenderScene() ? GetRenderScene()->GetFeatureLevel() : FSceneInterface::GetDefaultFeatureLevel();
		if (!LoadedDefaultMaterial && !Material && !GetMaterialOverride(0))
			LoadedDefaultMaterial = DefaultMaterialAsset.LoadSynchronous();
		if (auto* Asset = GetMaterial(0))
		{
			Diagnostic.clear();
			return Asset->GetRenderProxy(Feature);
		}
		constexpr std::string_view MissingMaterial = "Default Water material asset is unavailable; include /Plugins/Water/Materials/DefaultWater in the Cook.";
		if (Diagnostic != MissingMaterial) DURIN_ERROR("Water surface '{}': {}", GetObjectPath(), MissingMaterial);
		Diagnostic = MissingMaterial;
		return {};
	}

	auto DWaterSurfaceComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		if (!HasSupportedTransform())
		{
			Diagnostic = "Water supports positive uniform transform scale; use Extent to change its dimensions.";
			return {};
		}
		if (!Geometry) SynchronizeSurface();
		if (!Geometry) return {};
		auto Proxy = ResolveMaterial();
		if (!Proxy) return {};
		return std::make_unique<FWaterSceneProxy>(Geometry, std::move(Proxy), PackWaterInstanceWaveData(*EffectiveSettings));
	}

	auto DWaterSurfaceComponent::OnRegister() -> void
	{
		BindWaveAsset(WaveAsset.Get());
		SynchronizeSurface();
		Super::OnRegister();
	}
	auto DWaterSurfaceComponent::OnUnregister() -> void
	{
		Super::OnUnregister();
		BindWaveAsset(nullptr);
		Geometry.reset();
		EffectiveSettings.reset();
		LoadedDefaultMaterial = nullptr;
	}
	auto DWaterSurfaceComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		if (IsRegistered()) MarkRenderStateDirty();
	}
	auto DWaterSurfaceComponent::GetMaterialIndex(FName Name) const -> std::optional<uint32>
	{ return Name == FName("Water") ? std::optional<uint32>(0) : std::nullopt; }
	auto DWaterSurfaceComponent::GetDefaultMaterial(uint32 Index) const -> DMaterialInterface*
	{ return Index == 0 ? (Material ? Material.Get() : LoadedDefaultMaterial.Get()) : nullptr; }
	auto DWaterSurfaceComponent::BuildMaterialRenderProxyBindingUpdate(FMaterialRenderProxyBindingUpdate& Update) -> bool
	{
		Update.SlotIndex = 0; Update.MaterialProxy = ResolveMaterial();
		return bool(Update.MaterialProxy);
	}

	auto DWaterSurfaceComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
	{
		if (auto E = Super::PreEditChangeProperty(Proposal); !E) return E;
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("Settings")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty && Proposal.DraftRootContainer)
		{
			const auto* Candidate = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FWaterSurfaceSettings>(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (auto E = ValidateWaterSurface(*Candidate); !E) return RejectEnginePropertyEdit(*this, Proposal, E.error());
			if (WaveAsset)
				if (const auto Effective = ResolveWaterSettings(*Candidate, WaveAsset.Get()); !Effective)
					return RejectEnginePropertyEdit(*this, Proposal, Effective.error());
		}
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("WaveAsset")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty && Proposal.DraftRootContainer)
		{
			const auto& Candidate = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<TObjectPtr<DWaterWaves>>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (const auto Effective = ResolveWaterSettings(Settings, Candidate.Get()); !Effective)
				return RejectEnginePropertyEdit(*this, Proposal, Effective.error());
		}
		return {};
	}
	auto DWaterSurfaceComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || Event.MemberProperty->NamePrivate == FName("Settings")
			|| Event.MemberProperty->NamePrivate == FName("WaveAsset"))
		{
			RefreshSurface();
			return;
		}
		MarkRenderStateDirty();
	}
	auto DWaterSurfaceComponent::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError>
	{
		if (auto E = Super::ValidateLoadedObjectGraph(Context); !E) return E;
		if (auto E = ValidateWaterSurface(Settings); !E) return RejectLoadedObjectGraph(GetObjectPath(), E.error());
		return {};
	}
	auto DWaterSurfaceComponent::QuerySurface(FVector3 Point, double Seconds) const -> std::optional<FWaterSurfaceSample>
	{
		if (!HasSupportedTransform() || !std::isfinite(Point.x) || !std::isfinite(Point.y) || !std::isfinite(Point.z)) return {};
		const auto M = GetWorldTransform().ToMatrix();
		const FVector3 Local(Math::Inverse(M) * FVector4(Point, 1));
		const auto Query = [&](const FWaterSurfaceSettings& Effective) { return QueryWaterSurface(Effective, FVector2(Local), Seconds); };
		std::optional<FWaterSurfaceSample> Result;
		if (IsRegistered())
		{
			if (EffectiveSettings) Result = Query(*EffectiveSettings);
		}
		else if (const auto Effective = ResolveWaterSettings(Settings, WaveAsset.Get()))
		{
			Result = Query(*Effective);
		}
		if (!Result) return {};
		Result->Position = FVector3(M * FVector4(Result->Position, 1));
		Result->Normal = Math::RotateVector(GetWorldTransform().Rotation, Result->Normal);
		return Result;
	}
}
