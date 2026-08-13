#include "Renderers/ForwardLighting.h"

#include "Renderers/SceneVisibility.h"
#include "Renderers/ViewPreparationMath.h"
#include "Renderers/DirectionalShadowView.h"

#include "Math/Operations.h"
#include "Scene.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{
		auto IsValidColor(const FVector3f& Color) -> bool
		{
			return Math::IsFinite(Color) && Color.x >= 0.0f
				&& Color.y >= 0.0f && Color.z >= 0.0f;
		}

		auto IsValidDirection(const FVector3& Direction) -> bool
		{
			return Math::IsFinite(Direction)
				&& Math::LengthSquared(Direction) > 1.0e-8;
		}

		auto IsEnabled(float Intensity) -> bool
		{
			return std::isfinite(Intensity) && Intensity > 0.0f;
		}

		auto IsLocalVisible(
			const FLightSceneInfo& Info,
			const FSceneView& View,
			const FViewFrustum* Frustum) -> bool
		{
			if (View.Settings.VisibilityMode != EViewVisibilityMode::Normal
				|| Frustum == nullptr) return true;
			return ClassifyWorldBounds(*Frustum, Info.GetInfluenceBounds())
				!= EViewBoundsClassification::Outside;
		}
	}

	auto PrepareLightView_RenderThread(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderCounters& Counters) -> FPreparedLightView
	{
		FPreparedLightView Result;
		std::vector<FPreparedDirectionalLight> Directional;
		std::vector<FPreparedLocalLight> Local;
		FViewFrustum FrustumStorage;
		const FViewFrustum* Frustum = TryBuildViewFrustum(View, FrustumStorage)
			? &FrustumStorage : nullptr;

		for (const FLightSceneInfo* Info : Scene.GetDirectionalLightSceneInfos())
		{
			++Counters.SubmittedDirectionalLights;
			const auto& Data = Info->GetDirectionalProxy().GetData();
			if (!IsValidDirection(Data.Direction) || !IsValidColor(Data.Color)
				|| !IsEnabled(Data.Intensity))
			{
				++Counters.RejectedDirectionalLights;
				continue;
			}
			auto Copy = Data;
			Copy.Direction = Math::Normalize(Copy.Direction);
			Directional.push_back({Info->GetId(), Copy});
		}

		auto PrepareLocal = [&](const FLightSceneInfo& Info) {
			FPreparedLocalLight Copy;
			Copy.Id = Info.GetId();
			Copy.Kind = Info.GetKind();
			if (Info.GetKind() == ELightSceneProxyKind::Point)
			{
				++Counters.SubmittedPointLights;
				const auto& Data = Info.GetPointProxy().GetData();
				Copy.Position = Data.Position;
				Copy.Color = Data.Color;
				Copy.Intensity = Data.Intensity;
				Copy.Range = Data.Range;
			}
			else
			{
				++Counters.SubmittedSpotLights;
				const auto& Data = Info.GetSpotProxy().GetData();
				Copy.Position = Data.Position;
				Copy.Direction = Data.Direction;
				Copy.Color = Data.Color;
				Copy.Intensity = Data.Intensity;
				Copy.Range = Data.Range;
				Copy.InnerConeAngle = Data.InnerConeAngle;
				Copy.OuterConeAngle = Data.OuterConeAngle;
			}
			const bool bValid = Math::IsFinite(Copy.Position)
				&& IsValidColor(Copy.Color) && IsEnabled(Copy.Intensity)
				&& std::isfinite(Copy.Range) && Copy.Range > 0.0f
				&& (Copy.Kind == ELightSceneProxyKind::Point
					|| (IsValidDirection(Copy.Direction)
						&& std::isfinite(Copy.InnerConeAngle)
						&& std::isfinite(Copy.OuterConeAngle)
						&& Copy.InnerConeAngle >= 0.0f
						&& Copy.InnerConeAngle <= Copy.OuterConeAngle
						&& Copy.OuterConeAngle < 90.0f));
			if (!bValid)
			{
				if (Copy.Kind == ELightSceneProxyKind::Point)
					++Counters.RejectedPointLights;
				else ++Counters.RejectedSpotLights;
				return;
			}
			if (!IsLocalVisible(Info, View, Frustum))
			{
				if (Copy.Kind == ELightSceneProxyKind::Point)
					++Counters.FrustumCulledPointLights;
				else ++Counters.FrustumCulledSpotLights;
				return;
			}
			if (Copy.Kind == ELightSceneProxyKind::Spot)
				Copy.Direction = Math::Normalize(Copy.Direction);
			Local.push_back(Copy);
		};

		for (const FLightSceneInfo* Info : Scene.GetPointLightSceneInfos()) PrepareLocal(*Info);
		for (const FLightSceneInfo* Info : Scene.GetSpotLightSceneInfos()) PrepareLocal(*Info);
		std::ranges::sort(Directional, {}, [](const auto& Light) { return Light.Id.Value; });
		std::ranges::sort(Local, {}, [](const auto& Light) { return Light.Id.Value; });

		const size_t DirectionalCount = std::min<size_t>(Directional.size(), MaxPreparedDirectionalLights);
		Result.Directional.assign(Directional.begin(), Directional.begin() + DirectionalCount);
		Counters.SelectedDirectionalLights = DirectionalCount;
		Counters.OverflowDirectionalLights = Directional.size() - DirectionalCount;
		const size_t LocalCount = std::min<size_t>(Local.size(), MaxPreparedLocalLights);
		Result.Local.assign(Local.begin(), Local.begin() + LocalCount);
		for (size_t Index = 0; Index < Local.size(); ++Index)
		{
			const bool bSelected = Index < LocalCount;
			if (Local[Index].Kind == ELightSceneProxyKind::Point)
				(bSelected ? Counters.SelectedPointLights : Counters.OverflowPointLights)++;
			else (bSelected ? Counters.SelectedSpotLights : Counters.OverflowSpotLights)++;
		}
		Counters.PackedLightBytes = sizeof(FForwardLightingUniform);
		check(Counters.SubmittedDirectionalLights == Counters.RejectedDirectionalLights
			+ Counters.SelectedDirectionalLights + Counters.OverflowDirectionalLights);
		check(Counters.SubmittedPointLights == Counters.RejectedPointLights
			+ Counters.FrustumCulledPointLights + Counters.SelectedPointLights
			+ Counters.OverflowPointLights);
		check(Counters.SubmittedSpotLights == Counters.RejectedSpotLights
			+ Counters.FrustumCulledSpotLights + Counters.SelectedSpotLights
			+ Counters.OverflowSpotLights);
		return Result;
	}

	auto BuildForwardLightingUniform(
		const FPreparedLightView& Lights,
		const FSceneView& View,
		const FPreparedDirectionalShadowView* Shadow) -> FForwardLightingUniform
	{
		FForwardLightingUniform Result;
		Result.ViewPosition = FVector4f(FVector3f(View.ViewLocation), 0.0f);
		Result.Counts[0] = static_cast<uint32>(Lights.Directional.size());
		Result.Counts[1] = static_cast<uint32>(Lights.Local.size());
		if (!Lights.Directional.empty())
		{
			const auto& Light = Lights.Directional.front().Data;
			Result.Directional.Direction = FVector4f(FVector3f(Light.Direction), 0.0f);
			Result.Directional.ColorIntensity = FVector4f(Light.Color, Light.Intensity);
		}
		if (Shadow != nullptr && Shadow->bEnabled
			&& !Lights.Directional.empty()
			&& Shadow->LightId == Lights.Directional.front().Id)
		{
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					Result.DirectionalShadow.WorldToShadow[Column][Row] =
						static_cast<float>(Shadow->WorldToShadowMatrix[Row][Column]);
			Result.DirectionalShadow.Control = {
				1.0f, static_cast<float>(Shadow->DiagnosticMode),
				Shadow->Bias.bUsedFallback ? 1.0f : 0.0f,
				Shadow->Bias.bTotalClamped ? 1.0f : 0.0f};
			Result.DirectionalShadow.TexelBias = {
				static_cast<float>(Shadow->TexelWorldSize.x),
				static_cast<float>(Shadow->TexelWorldSize.y),
				Shadow->Bias.ReceiverWorld, Shadow->Bias.NormalWorld};
			Result.DirectionalShadow.RasterBias = {
				Shadow->Bias.RasterConstant, Shadow->Bias.RasterSlope,
				Shadow->Bias.RasterClamp,
				Shadow->Bias.NormalizedRasterSeparation};
			Result.DirectionalShadow.LightBounds = {
				static_cast<float>(Shadow->LightDirection.x),
				static_cast<float>(Shadow->LightDirection.y),
				static_cast<float>(Shadow->LightDirection.z),
				static_cast<float>(Shadow->Filter.GuardTexels)};
			Result.DirectionalShadow.Filter = {
				1.0f / static_cast<float>(DirectionalShadowResolution),
				1.0f / static_cast<float>(DirectionalShadowResolution),
				static_cast<float>(Shadow->Filter.Quality),
				Shadow->Filter.FootprintRadiusTexels};
		}
		for (size_t Index = 0; Index < Lights.Local.size(); ++Index)
		{
			const auto& Light = Lights.Local[Index];
			auto& Packed = Result.Local[Index];
			Packed.PositionInverseRange = FVector4f(
				FVector3f(Light.Position), 1.0f / Light.Range);
			Packed.DirectionType = FVector4f(
				FVector3f(Light.Direction),
				Light.Kind == ELightSceneProxyKind::Spot ? 1.0f : 0.0f);
			Packed.ColorIntensity = FVector4f(Light.Color, Light.Intensity);
			if (Light.Kind == ELightSceneProxyKind::Spot)
			{
				const float InnerCos = std::cos(glm::radians(Light.InnerConeAngle));
				const float OuterCos = std::cos(glm::radians(Light.OuterConeAngle));
				const float Denominator = InnerCos - OuterCos;
				Packed.SpotCone = FVector4f(
					InnerCos, OuterCos,
					Denominator > 1.0e-6f ? 1.0f / Denominator : 0.0f,
					Denominator > 1.0e-6f ? 0.0f : 1.0f);
			}
		}
		return Result;
	}
}
