#include "Components/SkyLightComponent.h"

#include "DObject/Property.h"
#include "Math/Operations.h"
#include "Engine/Actor.h"
#include "Rendering/SkyLightSceneProxy.h"
#include "SceneInterface.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextSkyLightInstance = 1;
	}

	DSkyLightComponent::DSkyLightComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, SkyLightSceneId(IsTemplateConstructionPurpose(ObjectInitializer.Purpose) ? FGuid{} : FGuid::NewGuid())
		, InstanceId(GNextSkyLightInstance.fetch_add(1, std::memory_order_relaxed))
		, UpdateStatus(std::make_shared<FSkyLightUpdateStatus>())
	{
		RefreshEligibilityDiagnostic();
	}

	auto DSkyLightComponent::OnRegister() -> void
	{
		Super::OnRegister();
		if (!SkyLightSceneId.IsValid()) SkyLightSceneId = FGuid::NewGuid();
		Publish();
	}

	auto DSkyLightComponent::OnUnregister() -> void
	{
		Remove();
		Super::OnUnregister();
	}

	auto DSkyLightComponent::OnOwnerVisibilityChanged() -> void { Publish(); }
	auto DSkyLightComponent::OnUpdateTransform() -> void { Super::OnUpdateTransform(); Publish(); }

	auto DSkyLightComponent::RefreshEligibilityDiagnostic() -> void
	{
		if (!bEnabled) EligibilityStatus = "Disabled";
		else if (GetOwner() && GetOwner()->IsHidden()) EligibilityStatus = "Owner is hidden";
		else if (SourceMode != ESkyLightSourceMode::SpecifiedCube && SourceMode != ESkyLightSourceMode::CapturedSky)
			EligibilityStatus = "Invalid source mode";
		else if (SourceMode == ESkyLightSourceMode::SpecifiedCube && !TextureCube) EligibilityStatus = "Assign an HDR TextureCube";
		else if (SourceMode == ESkyLightSourceMode::SpecifiedCube && !TextureCube->HasPlatformData()) EligibilityStatus = "Source platform data is pending";
		else if (SourceMode == ESkyLightSourceMode::SpecifiedCube
			&& (TextureCube->GetBuiltPixelFormat() != EPixelFormat::RGBA32_FLOAT
				|| TextureCube->GetBuiltFaceDimension() > 512)) EligibilityStatus = "Source requires linear HDR output at 512 pixels per face or less";
		else
		{
			switch (UpdateStatus->State.load(std::memory_order_acquire))
			{
			case ESkyLightUpdateState::Ready:
                EligibilityStatus = std::format("Ready; last GPU update {:.2f} ms; age {:.1f} s",
                    UpdateStatus->UpdateMilliseconds.load(),
                    std::max(0.0, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()
                        - UpdateStatus->CaptureTime.load()));
                break;
			case ESkyLightUpdateState::MissingProvider: EligibilityStatus = "No eligible Procedural Sky in this World"; break;
            case ESkyLightUpdateState::Backpressure: EligibilityStatus = "GPU image budget retained by in-flight work; retry pending"; break;
			case ESkyLightUpdateState::Failed: EligibilityStatus = "GPU update failed; retry pending"; break;
			case ESkyLightUpdateState::Unsupported: EligibilityStatus = "GPU sky lighting is unsupported"; break;
			default: EligibilityStatus = "Sky lighting update pending"; break;
			}
			if (SourceMode == ESkyLightSourceMode::CapturedSky)
				EligibilityStatus += ". Clouds, fog and geometry are excluded.";
		}
	}

	auto DSkyLightComponent::Publish() -> void
	{
		RefreshEligibilityDiagnostic();
		if (!IsRegistered()) return;
		if (auto* Scene = GetRenderScene())
		{
			Scene->RemoveSkyLight(this);
			Scene->AddSkyLight(this);
		}
	}

	auto DSkyLightComponent::Remove() -> void
	{
		if (IsRegistered())
			if (auto* Scene = GetRenderScene()) Scene->RemoveSkyLight(this);
	}

	auto DSkyLightComponent::CreateSceneProxy() -> std::shared_ptr<const FSkyLightSceneProxy>
	{
		if (TextureCube) (void)TextureCube->EnsurePlatformDataLoadedBlocking();
		auto Proxy = std::make_shared<FSkyLightSceneProxy>();
		Proxy->PersistentId = SkyLightSceneId;
		Proxy->SelectionKey = GetObjectPath();
		Proxy->InstanceId = InstanceId;
		Proxy->SourceEpoch = SourceEpoch;
		Proxy->RequestSerial = RequestSerial;
		Proxy->Priority = std::clamp(Priority, -1000, 1000);
		Proxy->SourceMode = SourceMode;
		Proxy->Texture = TextureCube ? TextureCube->GetTextureReferenceRHI() : FRHITextureReferenceRef{};
		const bool bValidRotation = Math::TryNormalize(GetWorldRotation(), Proxy->Rotation, 1.e-8);
		Proxy->Intensity = Intensity;
		Proxy->bAutomaticRefresh = bAutomaticRefresh;
		Proxy->RefreshInterval = RefreshInterval;
		Proxy->UpdateStatus = UpdateStatus;
		Proxy->bEligible = bValidRotation && bEnabled && (!GetOwner() || !GetOwner()->IsHidden())
			&& std::isfinite(Intensity) && Intensity >= 0 && Intensity <= 16
			&& std::isfinite(RefreshInterval) && RefreshInterval >= 0.25f && RefreshInterval <= 60
			&& (SourceMode == ESkyLightSourceMode::CapturedSky
				|| (SourceMode == ESkyLightSourceMode::SpecifiedCube && Proxy->Texture != nullptr));
		return Proxy;
	}

	auto DSkyLightComponent::SetSource(ESkyLightSourceMode Mode, DTextureCube* Cube) -> void
	{
		if (Mode != ESkyLightSourceMode::SpecifiedCube && Mode != ESkyLightSourceMode::CapturedSky) return;
		if (SourceMode == Mode && TextureCube.Get() == Cube) return;
		SourceMode = Mode;
		TextureCube = Cube;
		++SourceEpoch;
		UpdateStatus->State.store(ESkyLightUpdateState::Pending, std::memory_order_release);
		MarkPackageDirty();
		Publish();
	}

	auto DSkyLightComponent::SetIntensity(float Value) -> void
	{
		if (!std::isfinite(Value)) return;
		Intensity = std::clamp(Value, 0.0f, 16.0f);
		MarkPackageDirty();
		Publish();
	}

	auto DSkyLightComponent::SetPriority(int32 Value) -> void
	{
		Priority = std::clamp(Value, -1000, 1000);
		MarkPackageDirty();
		Publish();
	}

	auto DSkyLightComponent::SetEnabled(bool Value) -> void
	{
		bEnabled = Value;
		MarkPackageDirty();
		Publish();
	}

	auto DSkyLightComponent::SetRefreshPolicy(bool bAutomatic, float Interval) -> void
	{
		if (!std::isfinite(Interval)) return;
		bAutomaticRefresh = bAutomatic;
		RefreshInterval = std::clamp(Interval, 0.25f, 60.0f);
		MarkPackageDirty();
		Publish();
	}

	auto DSkyLightComponent::Recapture() -> void { ++RequestSerial; Publish(); }
	auto DSkyLightComponent::RefreshReloadedAssetBindings() -> void { ++SourceEpoch; Publish(); }

	auto DSkyLightComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || Proposal.DraftRootProperty != Proposal.MemberProperty
			|| !Proposal.DraftRootContainer) return true;
		const auto Name = Proposal.MemberProperty->NamePrivate;
		if (Name == FName("Intensity") || Name == FName("RefreshInterval"))
		{
			auto* Value = Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (!std::isfinite(*Value)) { OutError = "Sky Light values must be finite."; return false; }
			*Value = Name == FName("Intensity") ? std::clamp(*Value, 0.0f, 16.0f) : std::clamp(*Value, 0.25f, 60.0f);
		}
		if (Name == FName("Priority"))
		{
			auto* Value = Proposal.DraftRootProperty->ContainerPtrToValuePtr<int32>(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			*Value = std::clamp(*Value, -1000, 1000);
		}
		return true;
	}

	auto DSkyLightComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.Phase == EPropertyChangePhase::Committed && Event.Origin == EPropertyChangeOrigin::Edit) return;
		if (Event.MemberProperty && (Event.MemberProperty->NamePrivate == FName("SourceMode")
			|| Event.MemberProperty->NamePrivate == FName("TextureCube"))) ++SourceEpoch;
		Publish();
	}
}
