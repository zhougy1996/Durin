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

			// Use a specified integer-to-unit conversion rather than a standard
			// distribution whose mapping can differ between implementations.
			std::mt19937 Random(Settings.Seed);
			const auto Unit = [&] { return float(Random() >> 8) * (1.f / 16777216.f); };
			constexpr double Tau = 6.2831853071795864769;
			const float BaseWavelength = std::min(512.f, Settings.MinWavelength * 4.f);
			FWaterWaveGroup Group{0.f, BaseWavelength, {}};
			Group.Waves.reserve(Settings.NumWaves);
			double TotalAmplitude = 0;
			for (uint32 Index = 0; Index < Settings.NumWaves; ++Index)
			{
				const float Wavelength = std::lerp(Settings.MinWavelength, Settings.MaxWavelength, Unit());
				const float Amplitude = std::lerp(Settings.MinAmplitude, Settings.MaxAmplitude, Unit());
				const double Angle = Tau * Unit();
				Group.Waves.push_back({{std::cos(Angle), std::sin(Angle)}, Amplitude, BaseWavelength / Wavelength});
				TotalAmplitude += Amplitude;
			}
			if (TotalAmplitude > 2.)
				return std::unexpected("Generated wave amplitudes sum to more than 2. Reduce the amplitude range or wave count.");
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
