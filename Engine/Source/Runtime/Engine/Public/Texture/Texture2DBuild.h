#pragma once

#include "DerivedDataCacheKeyProxy.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DBuildProvider.h"

namespace Durin
{
	// Owned image mip chain and settings form the worker payload. Source identity
	// and cache policy belong to Engine orchestration, not the recipe provider.
	struct FTexture2DBuildRequest
	{
		// One source mip generates a chain; multiple source mips are preserved.
		std::vector<Image::FImage> SourceMips;
		FTexture2DBuildSettings Settings;
		FXxHash128 SourceIdentity;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
	};

	// Captures owned image values synchronously; no texture or package handles enter workers.
	ENGINE_API auto MakeTexture2DBuildRequest(const FTextureSource& Source,
		const FTexture2DBuildSettings& Settings = {}) -> FTexture2DBuildRequest;

	// Separates deterministic build/DDC identity from the Engine request serial
	// used to enforce latest-wins result application for one live object.
	struct FTexture2DBuildInputIdentity
	{
		FXxHash128 SourceIdentity;
		FTexture2DBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		FTexture2DBuildProviderDescriptor Provider;

		auto operator==(const FTexture2DBuildInputIdentity&) const -> bool = default;
	};

	// Identifies whether the provider returned cached data or ran the local recipe.
	enum class ETexture2DBuildProductOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	// Engine observations extend recipe timings with cache persistence.
	struct FTexture2DBuildMetrics : FTexture2DRecipeMetrics
	{
		uint64 PersistenceNanoseconds = 0;
	};

	// This observation/control value is borrowed only for the duration of Build.
	struct FTexture2DBuildExecutionControl
	{
		std::function<bool()> ShouldCancel;
		std::function<void()> OnPersisting;
		FTexture2DBuildMetrics* Metrics = nullptr;
	};

	// Detached Engine-owned CPU product. Applying it remains a separate
	// GameThread operation and does not execute provider code.
	struct FTexture2DBuildProduct
	{
		FTexturePlatformData PlatformData;
		FCacheKeyProxy DerivedDataKey;
		std::string PersistenceDiagnostic;
		FTexture2DBuildProviderDescriptor Provider;
		FTexture2DBuildMetrics Metrics;
		ETexture2DBuildProductOrigin Origin = ETexture2DBuildProductOrigin::Rebuilt;
	};

	// Invokes the single registered provider under its module-owned invocation
	// gate. The returned product and identity contain only Engine-owned values.
	ENGINE_API auto InvokeTexture2DBuildProvider(
		const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		FTexture2DBuildInputIdentity& OutIdentity,
		const FTexture2DBuildExecutionControl* ExecutionControl = nullptr) -> FTexture2DBuildResult;
}
