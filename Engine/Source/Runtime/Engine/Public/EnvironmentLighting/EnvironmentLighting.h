#pragma once

#include "CookedAsset.h"
#include "DObject/CoreDObject.h"
#include "EngineAPI.h"
#include "RHIDefinitions.h"

#include "EnvironmentLighting.gen.h"

namespace Durin
{
	inline constexpr uint32 EnvironmentIrradianceDimension = 16;
	inline constexpr uint32 EnvironmentPrefilterDimension = 64;
	inline constexpr uint32 EnvironmentPrefilterMipCount = 7;
	inline constexpr uint32 EnvironmentBrdfLutDimension = 128;
	inline constexpr uint32 EnvironmentLightingPayloadMagic = 0x504C4249; // IBLP
	inline constexpr uint32 EnvironmentLightingPayloadSchemaVersion = 1;
	inline constexpr uint32 DefaultStudioEnvironmentBuilderVersion = 1;
	inline constexpr uint32 EnvironmentLightingPayloadAlignment = 16;
	inline const FGuid EnvironmentLightingPrimaryCookedPayloadId{
		0x6ab24b59, 0x9bb94b7f, 0x831c596e, 0x1e822f9a};

	struct FEnvironmentLightingData
	{
		std::array<std::vector<uint16>, TextureCubeFaceCount> Irradiance;
		std::array<
			std::array<std::vector<uint16>, TextureCubeFaceCount>,
			EnvironmentPrefilterMipCount>
			Prefiltered;
		std::vector<uint16> BrdfLut;

		ENGINE_API auto IsValid() const -> bool;
		auto operator==(const FEnvironmentLightingData&) const -> bool = default;
	};

	ENGINE_API auto BuildDefaultStudioEnvironmentData() -> FEnvironmentLightingData;
	ENGINE_API auto EncodeEnvironmentLightingPayload(
		const FEnvironmentLightingData& Data,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
	ENGINE_API auto DecodeEnvironmentLightingPayload(
		std::span<const uint8> Bytes,
		std::shared_ptr<const FEnvironmentLightingData>& OutData,
		std::string& OutError) -> bool;

	DCLASS()
	class DEnvironmentLighting : public DObject
	{
		GENERATED_BODY()

	public:
		ENGINE_API explicit DEnvironmentLighting(const FObjectInitializer& ObjectInitializer);

		auto GetData() const -> const std::shared_ptr<const FEnvironmentLightingData>&
		{
			return Data;
		}
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor&
		{
			return CookedPayload;
		}

		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;

		ENGINE_API static auto GetAuthoringPayloadPath(std::string_view VirtualPackagePath)
			-> std::filesystem::path;

	private:
		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		std::shared_ptr<const FEnvironmentLightingData> Data;
	};
}
