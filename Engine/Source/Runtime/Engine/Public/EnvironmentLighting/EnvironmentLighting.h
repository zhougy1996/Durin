#pragma once

#include "Asset/BulkData.h"
#include "Asset/Cook.h"
#include "DObject/Object.h"
#include "EngineAPI.h"
#include "RHIDefinitions.h"

#include "EnvironmentLighting.gen.h"

namespace Durin
{
	inline constexpr uint32 EnvironmentIrradianceDimension = 16;
	inline constexpr uint32 EnvironmentPrefilterDimension = 64;
	inline constexpr uint32 EnvironmentPrefilterMipCount = 7;
	inline constexpr uint32 EnvironmentBrdfLutDimension = 128;
	inline constexpr uint32 EnvironmentLightingPayloadSchemaVersion = 2;
	inline constexpr uint32 DefaultStudioEnvironmentBuilderVersion = 2;
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
		ENGINE_API auto Serialize(FArchive& Ar) -> void;
		auto operator==(const FEnvironmentLightingData&) const -> bool = default;
	};

	ENGINE_API auto BuildDefaultStudioEnvironmentData() -> FEnvironmentLightingData;

	DCLASS()
	class DEnvironmentLighting : public DObject
	{
		GENERATED_BODY()

	public:
		ENGINE_API explicit DEnvironmentLighting(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto GetData() const
			-> const std::shared_ptr<const FEnvironmentLightingData>&;
		auto GetCookedPlatformData() const -> const Asset::FBulkData&
		{
			return CookedPlatformData;
		}

		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;

		ENGINE_API static auto GetAuthoredPayloadPath(std::string_view VirtualPackagePath)
			-> std::filesystem::path;

	private:
		DPROPERTY()
		uint32 PayloadSchemaVersion = EnvironmentLightingPayloadSchemaVersion;

		Asset::FBulkData CookedPlatformData;

		std::shared_ptr<const FEnvironmentLightingData> Data;
	};
}
