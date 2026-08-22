#include "Components/VolumetricCloudComponent.h"

#include "DObject/Property.h"
#include "Engine/Actor.h"
#include "Engine/VolumetricCloudSceneProxy.h"
#include "IScene.h"
#include "Texture/Texture2D.h"
#include "Texture/VolumeTexture.h"

#include <algorithm>
#include <cmath>

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextVolumetricCloudInstanceId = 1;

		template<typename T>
		auto ClampFinite(T Value, T Minimum, T Maximum, T& Out) -> bool
		{
			if (!std::isfinite(Value)) return false;
			Out = std::clamp(Value, Minimum, Maximum);
			return true;
		}

		auto ClampVector(FVector3f Value, float Minimum, float Maximum,
			FVector3f& Out) -> bool
		{
			return ClampFinite(Value.x, Minimum, Maximum, Out.x)
				&& ClampFinite(Value.y, Minimum, Maximum, Out.y)
				&& ClampFinite(Value.z, Minimum, Maximum, Out.z);
		}

		auto ClampVector(FVector2f Value, float Minimum, float Maximum,
			FVector2f& Out) -> bool
		{
			return ClampFinite(Value.x, Minimum, Maximum, Out.x)
				&& ClampFinite(Value.y, Minimum, Maximum, Out.y);
		}

		template<typename TTexture>
		auto GetValidTextureReference(TTexture* Texture) -> FRHITextureReferenceRef
		{
			if (Texture == nullptr || Texture->GetPlatformData() == nullptr
				|| !Texture->GetPlatformData()->IsValid()) return {};
			return Texture->GetTextureReferenceRHI();
		}
	}

	DVolumetricCloudComponent::DVolumetricCloudComponent(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, VolumetricCloudSceneId(IsTemplateConstructionPurpose(ObjectInitializer.Purpose)
			? FGuid{} : FGuid::NewGuid())
		, VolumetricCloudInstanceId(IsTemplateConstructionPurpose(ObjectInitializer.Purpose)
			? 0 : GNextVolumetricCloudInstanceId.fetch_add(1, std::memory_order_relaxed))
	{
	}

	auto DVolumetricCloudComponent::OnRegister() -> void
	{
		Super::OnRegister();
		if (!VolumetricCloudSceneId.IsValid())
			VolumetricCloudSceneId = FGuid::NewGuid();
		MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::OnUnregister() -> void
	{
		if (IScene* Scene = GetRenderScene(); PublicationRevision != 0 && Scene)
			Scene->RemoveVolumetricCloud(
				FVolumetricCloudSceneId(VolumetricCloudInstanceId), PublicationRevision);
		Super::OnUnregister();
	}

	auto DVolumetricCloudComponent::OnOwnerVisibilityChanged() -> void
	{
		MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::PreEditChangeProperty(
		FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || Proposal.DraftRootProperty != Proposal.MemberProperty
			|| !Proposal.DraftRootContainer) return true;
		const FName Name = Proposal.MemberProperty->NamePrivate;
		auto Reject = [&OutError] {
			OutError = "Volumetric cloud properties must be finite.";
			return false;
		};
		if (Name == FName("Priority"))
		{
			auto* Value = Proposal.DraftRootProperty->ContainerPtrToValuePtr<int32>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			*Value = std::clamp(*Value, -1000, 1000);
		}
		else if (Name == FName("MinimumZ") || Name == FName("MaximumZ")
			|| Name == FName("MaximumDistance"))
		{
			auto* Value = Proposal.DraftRootProperty->ContainerPtrToValuePtr<double>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			double Clamped = 0.0;
			const bool bDistance = Name == FName("MaximumDistance");
			if (!ClampFinite(*Value, bDistance ? 1.0 : -10'000'000.0,
				10'000'000.0, Clamped)) return Reject();
			*Value = Clamped;
		}
		else if (Name == FName("Coverage") || Name == FName("DetailErosion")
			|| Name == FName("Extinction") || Name == FName("LightExtinction")
			|| Name == FName("Ambient"))
		{
			auto* Value = Proposal.DraftRootProperty->ContainerPtrToValuePtr<float>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			float Clamped = 0.0f;
			if (!ClampFinite(*Value, 0.0f, 1.0f, Clamped)) return Reject();
			*Value = Clamped;
		}
		else if (Name == FName("BaseFrequency") || Name == FName("DetailFrequency")
			|| Name == FName("WindOffset"))
		{
			auto* Value = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FVector3f>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			FVector3f Clamped;
			const bool bFrequency = Name != FName("WindOffset");
			if (!ClampVector(*Value, bFrequency ? 0.00000001f : -1'000'000.0f,
				bFrequency ? 1.0f : 1'000'000.0f, Clamped)) return Reject();
			*Value = Clamped;
		}
		else if (Name == FName("WeatherFrequency") || Name == FName("WeatherOffset"))
		{
			auto* Value = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FVector2f>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			FVector2f Clamped;
			const bool bFrequency = Name == FName("WeatherFrequency");
			if (!ClampVector(*Value, bFrequency ? 0.00000001f : -1'000'000.0f,
				bFrequency ? 1.0f : 1'000'000.0f, Clamped)) return Reject();
			*Value = Clamped;
		}
		return true;
	}

	auto DVolumetricCloudComponent::PostEditChangeProperty(
		const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || (Event.Phase == EPropertyChangePhase::Committed
			&& Event.Origin == EPropertyChangeOrigin::Edit)) return;
		MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::SetEnabled(bool bInEnabled) -> void
	{
		if (bEnabled == bInEnabled) return;
		bEnabled = bInEnabled; MarkPackageDirty(); MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::SetPriority(int32 InPriority) -> void
	{
		const int32 Value = std::clamp(InPriority, -1000, 1000);
		if (Priority == Value) return;
		Priority = Value; MarkPackageDirty(); MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::SetBaseDensityTexture(DVolumeTexture* Texture) -> void
	{
		if (BaseDensityTexture.Get() == Texture) return;
		BaseDensityTexture = Texture; MarkPackageDirty(); MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::SetDetailDensityTexture(DVolumeTexture* Texture) -> void
	{
		if (DetailDensityTexture.Get() == Texture) return;
		DetailDensityTexture = Texture; MarkPackageDirty(); MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::SetWeatherTexture(DTexture2D* Texture) -> void
	{
		if (WeatherTexture.Get() == Texture) return;
		WeatherTexture = Texture; MarkPackageDirty(); MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::SetLayer(double InMinimumZ, double InMaximumZ,
		double InMaximumDistance) -> void
	{
		double NewMinimum = 0.0, NewMaximum = 0.0, NewDistance = 0.0;
		if (!ClampFinite(InMinimumZ, -10'000'000.0, 10'000'000.0, NewMinimum)
			|| !ClampFinite(InMaximumZ, -10'000'000.0, 10'000'000.0, NewMaximum)
			|| !ClampFinite(InMaximumDistance, 1.0, 10'000'000.0, NewDistance)) return;
		if (MinimumZ == NewMinimum && MaximumZ == NewMaximum
			&& MaximumDistance == NewDistance) return;
		MinimumZ = NewMinimum; MaximumZ = NewMaximum; MaximumDistance = NewDistance;
		MarkPackageDirty(); MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::SetDensityMapping(
		const FVector3f& InBaseFrequency, const FVector3f& InDetailFrequency,
		const FVector3f& InWindOffset, const FVector2f& InWeatherFrequency,
		const FVector2f& InWeatherOffset) -> void
	{
		FVector3f NewBase, NewDetail, NewWind;
		FVector2f NewWeatherFrequency, NewWeatherOffset;
		if (!ClampVector(InBaseFrequency, 0.00000001f, 1.0f, NewBase)
			|| !ClampVector(InDetailFrequency, 0.00000001f, 1.0f, NewDetail)
			|| !ClampVector(InWindOffset, -1'000'000.0f, 1'000'000.0f, NewWind)
			|| !ClampVector(InWeatherFrequency, 0.00000001f, 1.0f, NewWeatherFrequency)
			|| !ClampVector(InWeatherOffset, -1'000'000.0f, 1'000'000.0f, NewWeatherOffset)) return;
		if (BaseFrequency == NewBase && DetailFrequency == NewDetail
			&& WindOffset == NewWind && WeatherFrequency == NewWeatherFrequency
			&& WeatherOffset == NewWeatherOffset) return;
		BaseFrequency = NewBase; DetailFrequency = NewDetail; WindOffset = NewWind;
		WeatherFrequency = NewWeatherFrequency; WeatherOffset = NewWeatherOffset;
		MarkPackageDirty(); MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::SetOpticalProperties(float InCoverage,
		float InDetailErosion, float InExtinction, float InLightExtinction,
		float InAmbient) -> void
	{
		float NewCoverage = 0.0f, NewErosion = 0.0f, NewExtinction = 0.0f;
		float NewLightExtinction = 0.0f, NewAmbient = 0.0f;
		if (!ClampFinite(InCoverage, 0.0f, 1.0f, NewCoverage)
			|| !ClampFinite(InDetailErosion, 0.0f, 1.0f, NewErosion)
			|| !ClampFinite(InExtinction, 0.0f, 1.0f, NewExtinction)
			|| !ClampFinite(InLightExtinction, 0.0f, 1.0f, NewLightExtinction)
			|| !ClampFinite(InAmbient, 0.0f, 1.0f, NewAmbient)) return;
		if (Coverage == NewCoverage && DetailErosion == NewErosion
			&& Extinction == NewExtinction && LightExtinction == NewLightExtinction
			&& Ambient == NewAmbient) return;
		Coverage = NewCoverage; DetailErosion = NewErosion;
		Extinction = NewExtinction; LightExtinction = NewLightExtinction;
		Ambient = NewAmbient; MarkPackageDirty(); MarkVolumetricCloudRenderStateDirty();
	}

	auto DVolumetricCloudComponent::MarkVolumetricCloudRenderStateDirty() -> void
	{
		if (!IsRegistered()) return;
		IScene* Scene = GetRenderScene();
		if (Scene == nullptr || VolumetricCloudInstanceId == 0) return;
		FVolumetricCloudSceneData Data;
		Data.PersistentId = VolumetricCloudSceneId;
		Data.SelectionKey = GetObjectPath();
		Data.InstanceId = VolumetricCloudInstanceId;
		Data.PublicationRevision = ++PublicationRevision;
		Data.Priority = Priority;
		const AActor* Owner = GetOwner();
		Data.bEnabled = bEnabled && (!Owner || !Owner->IsHidden());
		Data.BaseDensityTexture = GetValidTextureReference(BaseDensityTexture.Get());
		Data.DetailDensityTexture = GetValidTextureReference(DetailDensityTexture.Get());
		Data.WeatherTexture = GetValidTextureReference(WeatherTexture.Get());
		Data.MinimumZ = MinimumZ; Data.MaximumZ = MaximumZ;
		Data.MaximumDistance = MaximumDistance;
		Data.BaseFrequency = BaseFrequency;
		Data.DetailFrequency = DetailFrequency;
		Data.WindOffset = WindOffset;
		Data.WeatherFrequency = WeatherFrequency;
		Data.WeatherOffset = WeatherOffset;
		Data.Coverage = Coverage; Data.DetailErosion = DetailErosion;
		Data.Extinction = Extinction; Data.LightExtinction = LightExtinction;
		Data.Ambient = Ambient;
		Data.bEligible = IsVolumetricCloudCandidateEligible(Data);
		Scene->AddOrReplaceVolumetricCloud(
			FVolumetricCloudSceneId(VolumetricCloudInstanceId), PublicationRevision,
			std::make_unique<FVolumetricCloudSceneProxy>(std::move(Data)));
	}
}
