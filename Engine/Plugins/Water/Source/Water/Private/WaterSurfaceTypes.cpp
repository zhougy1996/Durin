#include "WaterSurfaceTypes.h"

#include "CoreGlobals.h"
#include "Misc/Time.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		constexpr double Tau = 6.2831853071795864769;
		auto Finite(FVector2 V) -> bool { return std::isfinite(V.x) && std::isfinite(V.y); }
	}

	auto ValidateWaterSurface(const FWaterSurfaceSettings& S) -> std::expected<void, std::string>
	{
		if (!Finite(S.Extent) || !std::isfinite(S.Elevation) || !std::isfinite(S.Amplitude)
			|| !std::isfinite(S.Wavelength) || !std::isfinite(S.Speed) || !std::isfinite(S.Steepness))
			return std::unexpected("Water parameters must be finite.");
		if (S.Extent.x < 1 || S.Extent.y < 1 || S.Extent.x > 128 || S.Extent.y > 128
			|| std::abs(S.Elevation) > 10000 || S.Amplitude < 0 || S.Amplitude > 2
			|| S.Wavelength < 2 || S.Wavelength > 512 || S.Speed < 0 || S.Speed > 4
			|| S.Steepness < 0 || S.Steepness > 1)
			return std::unexpected("Water bounds: extent 1..128, elevation +/-10000, amplitude 0..2, wavelength 2..512, speed 0..4, steepness 0..1.");
		double Derivative = 0;
		for (const auto& W : WaterWaves) Derivative += S.Steepness * S.Amplitude * W.AmplitudeRatio * Tau * W.WavenumberRatio / S.Wavelength;
		if (Derivative > .5) return std::unexpected("Water horizontal wave derivative must not exceed 0.5; reduce amplitude/steepness or increase wavelength.");
		const double Step = S.Wavelength * .73 / 32;
		if (std::ceil(S.Extent.x / Step) > 256 || std::ceil(S.Extent.y / Step) > 256)
			return std::unexpected("Water grid requires more than 256 cells per axis; reduce extent or increase wavelength.");
		return {};
	}

	auto EvaluateWaterSurface(const FWaterSurfaceSettings& S, FVector2 P, double Seconds) -> FWaterSurfaceSample
	{
		FWaterSurfaceSample Result{FVector3(P, S.Elevation)};
		FVector3 TX(1, 0, 0), TY(0, 1, 0);
		for (const auto& W : WaterWaves)
		{
			const double A = S.Amplitude * W.AmplitudeRatio, K = Tau * W.WavenumberRatio / S.Wavelength;
			const double Omega = S.Speed * std::sqrt(9.81 * K);
			const double Phase = K * Math::Dot(W.Direction, P) - Omega * Seconds;
			const double Sin = std::sin(Phase), Cos = std::cos(Phase), QA = S.Steepness * A;
			Result.Position += FVector3(QA * W.Direction * Cos, A * Sin);
			TX += FVector3(-QA * K * W.Direction * W.Direction.x * Sin, A * K * W.Direction.x * Cos);
			TY += FVector3(-QA * K * W.Direction * W.Direction.y * Sin, A * K * W.Direction.y * Cos);
		}
		Result.Normal = Math::Normalize(Math::Cross(TX, TY));
		return Result;
	}

	auto QueryWaterSurface(const FWaterSurfaceSettings& S, FVector2 XY, double Seconds) -> std::optional<FWaterSurfaceSample>
	{
		if (!ValidateWaterSurface(S) || !Finite(XY) || !std::isfinite(Seconds)) return {};
		const double Envelope = S.Amplitude * S.Steepness;
		if (std::abs(XY.x) > S.Extent.x * .5 + Envelope || std::abs(XY.y) > S.Extent.y * .5 + Envelope) return {};
		FVector2 P = XY;
		for (uint32 Iteration = 0; Iteration < 40; ++Iteration)
		{
			const FVector2 Displaced(EvaluateWaterSurface(S, P, Seconds).Position);
			if (!Finite(Displaced)) return {};
			const FVector2 Next = P + XY - Displaced;
			if (Math::Dot(Next - P, Next - P) < 1.e-18) { P = Next; break; }
			P = Next;
		}
		if (std::abs(P.x) > S.Extent.x * .5 + 1.e-8 || std::abs(P.y) > S.Extent.y * .5 + 1.e-8) return {};
		return EvaluateWaterSurface(S, P, Seconds);
	}

	auto GetWaterMaterialTime() -> double { return FTime::Seconds() - GStartTime; }
}
