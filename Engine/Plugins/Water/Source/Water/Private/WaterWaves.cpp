#include "WaterWaves.h"

#include "Components/PropertyEditValidation.h"
#include "DObject/Property.h"

namespace Durin
{
	namespace
	{
		auto GenerateWaveGroup(const FWaterWavesSettings& Settings) -> std::expected<FWaterWaveGroup, std::string>
		{
			if (Settings.NumWaves > MaxWaterWaves)
				return std::unexpected(std::format("Water Waves supports at most {} Gerstner waves.", MaxWaterWaves));
			if (!std::isfinite(Settings.MinWavelength) || !std::isfinite(Settings.MaxWavelength)
				|| Settings.MinWavelength < 2.f || Settings.MinWavelength > Settings.MaxWavelength || Settings.MaxWavelength > 512.f
				|| Settings.MaxWavelength > 32.f * Settings.MinWavelength)
				return std::unexpected("Wavelengths must form an ordered range within 2..512, with a maximum at most 32 times the minimum.");
			if (!std::isfinite(Settings.MinAmplitude) || !std::isfinite(Settings.MaxAmplitude)
				|| Settings.MinAmplitude < 0.f || Settings.MinAmplitude > Settings.MaxAmplitude || Settings.MaxAmplitude > 2.f)
				return std::unexpected("Amplitudes must form an ordered range within 0..2.");

			if (!std::isfinite(Settings.Randomness) || Settings.Randomness < 0
				|| !std::isfinite(Settings.WavelengthFalloff) || Settings.WavelengthFalloff < 0
				|| !std::isfinite(Settings.AmplitudeFalloff) || Settings.AmplitudeFalloff < 0
				|| !std::isfinite(Settings.WindAngleDeg) || !std::isfinite(Settings.DirectionAngularSpreadDeg)
				|| Settings.DirectionAngularSpreadDeg < 0)
				return std::unexpected("Wave falloffs, randomness and spread must be finite and nonnegative, and wind angle finite.");
			// FRandomStream consumes one sample for alpha even with zero randomness.
			uint32 State = Settings.Seed;
			auto SignedUnit = [&] {
				State = State * 196314165u + 907633515u;
				return float(State >> 9) * (2.f / 8388608.f) - 1.f;
			};
			// Keep the component's historical base wavelength wherever all ratios fit.
			const float BaseWavelength = std::clamp(8.f, Settings.MaxWavelength * .125f, Settings.MinWavelength * 4.f);
			FWaterWaveGroup Group{0.f, BaseWavelength, {}};
			Group.Waves.reserve(Settings.NumWaves);
			double TotalAmplitude = 0;
			for (uint32 Index = 0; Index < Settings.NumWaves; ++Index)
			{
				const float Alpha = std::clamp(1.f - float(Index) / Settings.NumWaves
					+ SignedUnit() * (Settings.Randomness / Settings.NumWaves), 0.f, 1.f);
				const float Wavelength = std::lerp(Settings.MinWavelength, Settings.MaxWavelength, std::pow(Alpha, Settings.WavelengthFalloff));
				const float Amplitude = std::lerp(Settings.MinAmplitude, Settings.MaxAmplitude, std::pow(Alpha, Settings.AmplitudeFalloff));
				const double Angle = (double(Settings.WindAngleDeg) + (Index ? double(SignedUnit()) * Settings.DirectionAngularSpreadDeg : 0.)) * (3.141592653589793 / 180.);
				Group.Waves.push_back({{std::cos(Angle), std::sin(Angle)}, Amplitude, BaseWavelength / Wavelength});
				TotalAmplitude += Amplitude;
			}
			Group.BaseAmplitude = float(TotalAmplitude);
			if (TotalAmplitude > 0)
				for (auto& Wave : Group.Waves) Wave.AmplitudeRatio = float(Wave.AmplitudeRatio / TotalAmplitude);
			return Group;
		}
	}

	DWaterWaves::DWaterWaves(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DWaterWaves::SetGeneratorSettings(const FWaterWavesSettings& Value) -> std::expected<void, std::string>
	{
		if (Settings == Value) return {};
		auto Group = GenerateWaveGroup(Value);
		if (!Group) return std::unexpected(Group.error());
		Settings = Value;
		GeneratedGroup = std::move(Group);
		GeneratedFrom = Settings;
		MarkPackageDirty();
		WavesChanged.Broadcast();
		return {};
	}

	auto DWaterWaves::GetWaveGroup() const -> const std::expected<FWaterWaveGroup, std::string>&
	{
		if (GeneratedFrom != Settings)
		{
			GeneratedGroup = GenerateWaveGroup(Settings);
			GeneratedFrom = Settings;
		}
		return GeneratedGroup;
	}

	auto DWaterWaves::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError>
	{
		if (auto Result = Super::PreEditChangeProperty(Proposal); !Result) return Result;
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("Settings")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty && Proposal.DraftRootContainer)
		{
			const auto& Candidate = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<FWaterWavesSettings>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (const auto Group = GenerateWaveGroup(Candidate); !Group) return RejectEnginePropertyEdit(*this, Proposal, Group.error());
		}
		return {};
	}

	auto DWaterWaves::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty || Event.MemberProperty->NamePrivate == FName("Settings")) WavesChanged.Broadcast();
	}

	auto DWaterWaves::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError>
	{
		if (auto Result = Super::ValidateLoadedObjectGraph(Context); !Result) return Result;
		if (const auto Group = GenerateWaveGroup(Settings); !Group) return RejectLoadedObjectGraph(GetObjectPath(), Group.error());
		return {};
	}
}
