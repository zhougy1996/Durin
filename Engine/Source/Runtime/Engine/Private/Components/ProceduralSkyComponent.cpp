#include "Components/ProceduralSkyComponent.h"

#include "DObject/Property.h"
#include "Engine/Actor.h"
#include "Math/Operations.h"
#include "Rendering/ProceduralSkySceneProxy.h"
#include "SceneInterface.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextProceduralSkyInstance = 1;
		auto Finite(const FVector3f& V) -> bool
		{
			return std::isfinite(V.x) && std::isfinite(V.y) && std::isfinite(V.z);
		}
		auto Color(const FVector3f& V) -> FVector3f
		{
			return {std::clamp(V.x, 0.0f, 16.0f), std::clamp(V.y, 0.0f, 16.0f), std::clamp(V.z, 0.0f, 16.0f)};
		}
	}

	DProceduralSkyComponent::DProceduralSkyComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, ProceduralSkySceneId(IsTemplateConstructionPurpose(ObjectInitializer.Purpose) ? FGuid{} : FGuid::NewGuid())
		, InstanceId(GNextProceduralSkyInstance.fetch_add(1, std::memory_order_relaxed)) {}

	auto DProceduralSkyComponent::OnRegister() -> void
	{
		Super::OnRegister();
		if (!ProceduralSkySceneId.IsValid()) ProceduralSkySceneId = FGuid::NewGuid();
		Publish();
	}

	auto DProceduralSkyComponent::OnUnregister() -> void
	{
		if (auto* Scene = GetRenderScene()) Scene->RemoveProceduralSky(this);
		Super::OnUnregister();
	}

	auto DProceduralSkyComponent::OnOwnerVisibilityChanged() -> void { Publish(); }
	auto DProceduralSkyComponent::OnUpdateTransform() -> void { Super::OnUpdateTransform(); Publish(); }
	auto DProceduralSkyComponent::Publish() -> void
	{
		++Revision;
		if (IsRegistered())
			if (auto* Scene = GetRenderScene())
			{
				Scene->RemoveProceduralSky(this);
				Scene->AddProceduralSky(this);
			}
	}

	auto DProceduralSkyComponent::SetSunDirection(const FVector3f& Direction) -> void
	{
		if (!Finite(Direction) || Math::Dot(Direction, Direction) < 1.0e-12f) return;
		SunDirection = Math::Normalize(Direction);
		MarkPackageDirty();
		Publish();
	}
    auto DProceduralSkyComponent::SetRadianceColors(const FVector3f& Zenith, const FVector3f& Horizon,
        const FVector3f& Ground, const FVector3f& Halo) -> void
    {
        if (!Finite(Zenith) || !Finite(Horizon) || !Finite(Ground) || !Finite(Halo)) return;
        ZenithColor=Color(Zenith); HorizonColor=Color(Horizon); GroundColor=Color(Ground); HaloColor=Color(Halo);
        MarkPackageDirty(); Publish();
    }
	auto DProceduralSkyComponent::SetExposure(float Value) -> void
	{
		if (!std::isfinite(Value)) return;
		ExposureEV = std::clamp(Value, -8.0f, 8.0f);
		MarkPackageDirty();
		Publish();
	}
	auto DProceduralSkyComponent::SetEnabled(bool Value) -> void { bEnabled = Value; MarkPackageDirty(); Publish(); }
	auto DProceduralSkyComponent::SetPriority(int32 Value) -> void { Priority = std::clamp(Value, -1000, 1000); MarkPackageDirty(); Publish(); }

	auto DProceduralSkyComponent::CreateSceneProxy() -> std::shared_ptr<const FProceduralSkySceneProxy>
	{
		auto Proxy = std::make_shared<FProceduralSkySceneProxy>();
		Proxy->PersistentId = ProceduralSkySceneId;
		Proxy->SelectionKey = GetObjectPath();
		Proxy->InstanceId = InstanceId;
		Proxy->Revision = Revision;
		Proxy->Priority = std::clamp(Priority, -1000, 1000);
		Proxy->bEligible = bEnabled && (!GetOwner() || !GetOwner()->IsHidden())
			&& Finite(SunDirection) && Math::Dot(SunDirection, SunDirection) > 1.0e-12f
			&& Finite(ZenithColor) && Finite(HorizonColor) && Finite(GroundColor) && Finite(HaloColor) && Finite(Tint)
			&& std::isfinite(ExposureEV) && std::isfinite(HorizonExponent) && std::isfinite(HaloExponent);
		if (!Proxy->bEligible) return Proxy;
		auto& P = Proxy->Parameters;
		P.ToSun = FVector3f(GetWorldRotation() * FVector3(Math::Normalize(SunDirection)));
		P.Up = FVector3f(GetWorldRotation() * FVector3(0, 0, 1));
		P.Zenith = Color(ZenithColor);
		P.Horizon = Color(HorizonColor);
		P.Ground = Color(GroundColor);
		P.Halo = Color(HaloColor);
		P.Tint = Color(Tint);
		P.ExposureEV = std::clamp(ExposureEV, -8.0f, 8.0f);
		P.HorizonExponent = std::clamp(HorizonExponent, 0.25f, 8.0f);
		P.HaloExponent = std::clamp(HaloExponent, 1.0f, 64.0f);
		return Proxy;
	}

	auto DProceduralSkyComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || Proposal.DraftRootProperty != Proposal.MemberProperty || !Proposal.DraftRootContainer) return true;
		const auto Name = Proposal.MemberProperty->NamePrivate;
		auto Reject = [&] { OutError = "Sky parameters must be finite, with a nonzero sun direction."; return false; };
		if (Name == FName("SunDirection") || Name == FName("ZenithColor") || Name == FName("HorizonColor")
			|| Name == FName("GroundColor") || Name == FName("HaloColor") || Name == FName("Tint"))
		{
			auto* V = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FVector3f>(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (!Finite(*V)) return Reject();
			if (Name == FName("SunDirection"))
			{
				if (Math::Dot(*V, *V) < 1.0e-12f) return Reject();
				*V = Math::Normalize(*V);
			}
			else *V = Color(*V);
		}
		if (Name == FName("ExposureEV") || Name == FName("HorizonExponent") || Name == FName("HaloExponent"))
		{
			auto* V = Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (!std::isfinite(*V)) return Reject();
			*V = Name == FName("ExposureEV") ? std::clamp(*V, -8.0f, 8.0f)
				: Name == FName("HorizonExponent") ? std::clamp(*V, 0.25f, 8.0f) : std::clamp(*V, 1.0f, 64.0f);
		}
		return true;
	}

	auto DProceduralSkyComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.Phase != EPropertyChangePhase::Committed || Event.Origin != EPropertyChangeOrigin::Edit) Publish();
	}
}
