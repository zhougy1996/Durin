#include "WaterSurfaceMetrics.h"

#include "CoreGlobals.h"
#include "Misc/Time.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		constexpr double Tau = 6.2831853071795864769;
		auto Finite(FVector2 V) -> bool { return std::isfinite(V.x) && std::isfinite(V.y); }

		// Both evaluators consume the same normalized Float32 direction.
		auto WaveDirection(FVector2 Direction) -> FVector2f
		{
			const FVector2 Scaled = Direction / std::max(std::abs(Direction.x), std::abs(Direction.y));
			return FVector2f(Scaled / std::sqrt(Math::Dot(Scaled, Scaled)));
		}
	}

	auto MeasureWaterSurface(const FResolvedWaterSurface& S) -> std::expected<FWaterSurfaceMetrics, std::string>
	{
		if (!Finite(S.Extent) || !std::isfinite(S.Elevation) || !std::isfinite(S.Amplitude)
			|| !std::isfinite(S.Wavelength) || !std::isfinite(S.Speed) || !std::isfinite(S.Steepness))
			return std::unexpected("Water parameters must be finite.");
		if (S.Extent.x < 1 || S.Extent.y < 1 || S.Extent.x > 128 || S.Extent.y > 128
			|| std::abs(S.Elevation) > 10000 || S.Amplitude < 0 || S.Amplitude > 2 * MaxWaterWaves
			|| S.Wavelength < 2 || S.Wavelength > 512 || S.Speed < 0 || S.Speed > 4
			|| S.Steepness < 0 || S.Steepness > 1)
			return std::unexpected("Water bounds: extent 1..128, elevation +/-10000, amplitude 0..32, wavelength 2..512, speed 0..4, steepness 0..1.");
		if (S.Waves.size() > MaxWaterWaves)
			return std::unexpected(std::format("Water supports at most {} Gerstner waves.", MaxWaterWaves));

		double Derivative = 0;
		double AmplitudeRatioSum = 0;
		double MaxWavenumberRatio = 0;
		for (const auto& Wave : S.Waves)
		{
			if (!Finite(Wave.Direction) || (Wave.Direction.x == 0 && Wave.Direction.y == 0)
				|| !std::isfinite(Wave.AmplitudeRatio) || Wave.AmplitudeRatio < 0 || Wave.AmplitudeRatio > 1
				|| !std::isfinite(Wave.WavenumberRatio) || Wave.WavenumberRatio < .125f || Wave.WavenumberRatio > 4)
				return std::unexpected("Water waves require a finite nonzero direction, amplitude ratio 0..1 and wavenumber ratio 0.125..4.");
			if (Wave.AmplitudeRatio == 0) continue;

			AmplitudeRatioSum += Wave.AmplitudeRatio;
			MaxWavenumberRatio = std::max(MaxWavenumberRatio, double(Wave.WavenumberRatio));
			const FVector2 Direction(WaveDirection(Wave.Direction));
			Derivative += double(S.Steepness) * S.Amplitude * Wave.AmplitudeRatio * Tau * Wave.WavenumberRatio
				/ S.Wavelength * Math::Dot(Direction, Direction);
		}
		// Authored weights are Float32, including the legacy 0.6/0.3/0.1 group.
		if (AmplitudeRatioSum > 1.000001)
			return std::unexpected("Water wave amplitude ratios must sum to at most 1.");
		if (Derivative > .5) return std::unexpected("Water horizontal wave derivative must not exceed 0.5; reduce amplitude/steepness or increase wavelength.");

		// Keep the legacy grid spacing for a group whose highest ratio is 1.37.
		const double SamplesPerUnit = MaxWavenumberRatio * (32 / (.73 * 1.37f)) / S.Wavelength;
		const double CellsX = std::max(1., std::ceil(S.Extent.x * SamplesPerUnit));
		const double CellsY = std::max(1., std::ceil(S.Extent.y * SamplesPerUnit));
		if (CellsX > 256 || CellsY > 256)
			return std::unexpected("Water grid requires more than 256 cells per axis; reduce extent or increase wavelength.");
		const double Amplitude = S.Amplitude * AmplitudeRatioSum;
		return FWaterSurfaceMetrics{uint32(CellsX), uint32(CellsY), Amplitude, Amplitude * S.Steepness};
	}

	auto ValidateWaterSurface(const FResolvedWaterSurface& S) -> std::expected<void, std::string>
	{
		if (const auto Metrics = MeasureWaterSurface(S); !Metrics) return std::unexpected(Metrics.error());
		return {};
	}

	auto PackWaterWaveData(const FResolvedWaterSurface& S) -> FWaterWaveData
	{
		FWaterWaveData Data{S.Amplitude, S.Wavelength, S.Speed, S.Steepness};
		for (size_t Index = 0; Index < std::min<size_t>(PrimitiveWaterWaves, S.Waves.size()); ++Index)
		{
			const auto& Wave = S.Waves[Index];
			if (Wave.AmplitudeRatio == 0) continue;
			const auto Direction = WaveDirection(Wave.Direction);
			const size_t Slot = WaterWaveDataStart + Index * WaterWaveDataStride;
			Data[Slot] = Direction.x;
			Data[Slot + 1] = Direction.y;
			Data[Slot + 2] = Wave.AmplitudeRatio;
			Data[Slot + 3] = Wave.WavenumberRatio;
		}
		return Data;
	}

	auto PackWaterInstanceWaveData(const FResolvedWaterSurface& S) -> FWaterInstanceWaveData
	{
		FWaterInstanceWaveData Data{};
		for (size_t Index = PrimitiveWaterWaves; Index < S.Waves.size(); ++Index)
		{
			const auto& Wave = S.Waves[Index];
			if (Wave.AmplitudeRatio == 0) continue;
			const size_t Slot = (Index - PrimitiveWaterWaves) * InstanceWaterWaveStride;
			Data[Slot] = float(std::atan2(Wave.Direction.y, Wave.Direction.x));
			Data[Slot + 1] = Wave.AmplitudeRatio;
			Data[Slot + 2] = Wave.WavenumberRatio;
		}
		return Data;
	}

	auto EvaluateWaterSurface(const FResolvedWaterSurface& S, FVector2 P, double Seconds) -> FWaterSurfaceSample
	{
		FWaterSurfaceSample Result{FVector3(P, S.Elevation)};
		FVector3 TX(1, 0, 0), TY(0, 1, 0);
		for (const auto& W : S.Waves)
		{
			if (W.AmplitudeRatio == 0) continue;
			const FVector2 Direction(WaveDirection(W.Direction));
			const double A = double(S.Amplitude) * W.AmplitudeRatio, K = Tau * W.WavenumberRatio / S.Wavelength;
			const double Omega = S.Speed * std::sqrt(9.81 * K);
			const double Phase = K * Math::Dot(Direction, P) - Omega * Seconds;
			const double Sin = std::sin(Phase), Cos = std::cos(Phase), QA = S.Steepness * A;
			Result.Position += FVector3(QA * Direction * Cos, A * Sin);
			TX += FVector3(-QA * K * Direction * Direction.x * Sin, A * K * Direction.x * Cos);
			TY += FVector3(-QA * K * Direction * Direction.y * Sin, A * K * Direction.y * Cos);
		}
		Result.Normal = Math::Normalize(Math::Cross(TX, TY));
		return Result;
	}

	auto QueryWaterSurface(const FResolvedWaterSurface& S, FVector2 XY, double Seconds) -> std::optional<FWaterSurfaceSample>
	{
		if (!Finite(XY) || !std::isfinite(Seconds)) return {};
		const auto Metrics = MeasureWaterSurface(S);
		if (!Metrics) return {};
		const double Envelope = Metrics->HorizontalEnvelope + 1.e-8;
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
