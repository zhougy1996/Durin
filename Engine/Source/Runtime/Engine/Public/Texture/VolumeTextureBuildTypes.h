#pragma once

#include "Texture/VolumeTextureData.h"
#include "Texture/TextureBuildOutcome.h"

namespace Durin
{
	// Stable producer identity included in Engine-side diagnostics and contracts.
	struct FVolumeTextureBuildDescriptor
	{
		std::string ProducerIdentity;
		uint32 BuilderVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty() && BuilderVersion != 0;
		}
	};

	struct FVolumeTextureRecipeBuildRequest
	{
		std::reference_wrapper<const FVolumeTextureSourceData> SourceData;
		FVolumeTextureBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
	};

	struct FVolumeTextureRecipeBuildProduct
	{
		std::unique_ptr<FVolumeTexturePlatformData> PlatformData;
	};

}
