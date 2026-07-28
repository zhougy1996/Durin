#pragma once

#include "StaticModelImportBuild.h"

namespace Durin
{
	enum class EImportTransactionFailurePoint : uint8
	{
		None,
		DirectoryCreation,
		SourceWrite,
		SourcePublication,
		Decode,
		TextureBuild,
		DerivedDataPublication,
		PackageStaging,
		DependencyPackagePublication,
		RegistryPublication,
		RootPackagePublication
	};

	struct ENGINEASSETBUILD_API FMultiAssetImportTransactionTestAccess
	{
		static auto SetFailurePoint(
			FMultiAssetImportTransaction& Transaction,
			EImportTransactionFailurePoint Point,
			size_t Occurrence = 0) -> void;
	};

	struct FTextureBuildOperations
	{
		using FBeforePhase = std::function<bool(
			EImportTransactionFailurePoint,
			std::string&)>;

		static auto Build(
			DTexture2D& Texture,
			std::span<const uint8> EncodedBytes,
			const FSourcePath& SourcePath,
			const FTexture2DImportSettings& Settings,
			const FBeforePhase& BeforePhase,
			std::string& OutError) -> bool
		{
			const DTexture2D::FEncodedBuildHooks Hooks{
				.BeforeDecode = [&](std::string& Error) {
					return BeforePhase(EImportTransactionFailurePoint::Decode, Error);
				},
				.BeforeTextureBuild = [&](std::string& Error) {
					return BeforePhase(EImportTransactionFailurePoint::TextureBuild, Error);
				},
				.BeforeDerivedDataPublication = [&](std::string& Error) {
					return BeforePhase(
						EImportTransactionFailurePoint::DerivedDataPublication, Error);
				}};
			return Texture.BuildFromEncodedBytes(
				EncodedBytes, SourcePath, Settings, &Hooks, OutError);
		}
	};

	struct ENGINEASSETBUILD_API FStaticModelImportExecutionTestAccess
	{
		static auto SetFailurePoint(
			FStaticModelImportPlan& Plan,
			EImportTransactionFailurePoint Point,
			size_t Occurrence = 0) -> void;
	};
}
