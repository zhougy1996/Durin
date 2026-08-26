#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "AssetForge/ImportRequest.h"
#include "AssetForge/Operations/ImportExecution.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DAuthoring.h"

namespace Durin::AssetForge::Builtins
{
	ASSETFORGEBUILTINS_API auto IsTexture2DSourceExtension(
		std::string_view Extension) -> bool;
	// Translates one concrete encoded image into Engine's normalized RGBA8 source value.
	ASSETFORGEBUILTINS_API auto TranslateTexture2DSource(
		std::span<const std::byte> EncodedBytes,
		FTextureSourceData& OutSourceData,
		std::string& OutError) -> bool;

	// Builds the generic framework request used by Texture2D import, preview,
	// reimport, replacement, repair, and recovery entrypoints.
	ASSETFORGEBUILTINS_API auto MakeTexture2DImportRequest(
		const FSourcePath& MountedSource,
		const FAssetPath& Destination,
		const FTexture2DImportSettings& Settings,
		EImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FImportProvenance> ExistingProvenance,
		FImportRequest& OutRequest,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto InspectTexture2DImportProvenance(
		const DTexture2D& Texture,
		FImportProvenance& OutProvenance,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SubmitTexture2DImport(
		std::string_view FilePath, const FAssetPath& Destination,
		const FTexture2DImportSettings& Settings, bool bEngineAuthoringContext,
		FImportCompletion Completion,
		std::string& OutError) -> FImportHandle;
	// Ingests an external source and executes the generic request inline. The
	// returned framework outcome is the UI-facing submission contract.
	ASSETFORGEBUILTINS_API auto ImportTexture2D(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings = {},
		bool bEngineAuthoringContext = false) -> FImportResult;

	// Standard image-provider adapter: translate, build a detached product, then
	// publish it to a main-thread candidate object.

	ASSETFORGEBUILTINS_API auto ImportTexture2DAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTexture2DImportSettings& Settings = {},
		bool bEngineAuthoringContext = false) -> FTexture2DImportResult;

	// Submits a rebuild from retained source; completion runs on the game thread
	// after the candidate state is either published or rejected.
	ASSETFORGEBUILTINS_API auto ReimportTexture2DSource(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError,
		Asset::FTexture2DAuthoringCompletion Completion = {}) -> bool;
	// Rebuilds one packaged texture from its retained mounted source without
	// publishing the proposed settings until asynchronous preparation succeeds.
	ASSETFORGEBUILTINS_API auto RebuildTexture2DFromCurrentSource(
		DTexture2D& Texture,
		const Asset::FTexture2DBuildSettings& Settings,
		std::string& OutError,
		Asset::ETexture2DBuildPriority Priority =
			Asset::ETexture2DBuildPriority::Interactive,
		Asset::FTexture2DAuthoringCompletion Completion = {}) -> bool;
	ASSETFORGEBUILTINS_API auto ChangeTexture2DSourceReference(
		DTexture2D& Texture,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto IngestAndChangeTexture2DSource(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto RepairTexture2DSourcePath(
		DTexture2D& Texture,
		std::string_view FilePath,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto ChangeTexture2DSourceLocation(
		DTexture2D& Texture,
		std::string_view SourceDestination,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DUsage(
		DTexture2D& Texture, ETextureUsage Usage, std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DSRGB(
		DTexture2D& Texture, bool bSRGB, std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DMaxResolution(
		DTexture2D& Texture, uint32 MaxResolution, std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DCompressionQuality(
		DTexture2D& Texture,
		ETextureCompressionQuality Quality,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DAlphaMipMode(
		DTexture2D& Texture, ETextureAlphaMipMode Mode, std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SetTexture2DAlphaCoverageThreshold(
		DTexture2D& Texture, float Threshold, std::string& OutError) -> bool;
}
