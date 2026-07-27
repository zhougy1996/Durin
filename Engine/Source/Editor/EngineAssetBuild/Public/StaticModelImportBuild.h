#pragma once

#include "EngineAssetBuildAPI.h"
#include "DObject/AssetPath.h"
#include "Source/SourcePath.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	enum class EImportTransactionFailurePoint : uint8
	{
		None,
		DirectoryCreation,
		SourceWrite,
		Decode,
		TextureBuild,
		DerivedDataPublication,
		PackageSave,
		RegistryPublication,
		RootPackageSave
	};

	struct FPortableTextureBuildRequest
	{
		FAssetPath AssetPath;

		// Exactly one of ExternalSource and EncodedBytes must be populated.
		// Mounted external sources are referenced in place. Other external
		// sources and embedded bytes require an explicit writable destination.
		std::filesystem::path ExternalSource;
		std::vector<uint8> EncodedBytes;
		FSourcePath SourceDestination;
		FTexture2DImportSettings Settings;
		bool bRootPackage = false;
	};

	struct FImportTransactionResult
	{
		bool bSucceeded = false;
		std::string Message;
		std::vector<DTexture2D*> Textures;

		explicit operator bool() const { return bSucceeded; }
	};

	ENGINEASSETBUILD_API auto BuildEmbeddedImageSourcePath(
		const FSourcePath& RootModelSource,
		std::string_view ImageIdentity,
		std::string_view SuggestedName,
		std::string_view Extension,
		FSourcePath& OutPath,
		std::string& OutError) -> bool;

	class ENGINEASSETBUILD_API FMultiAssetImportTransaction
	{
	public:
		FMultiAssetImportTransaction();
		~FMultiAssetImportTransaction();

		FMultiAssetImportTransaction(const FMultiAssetImportTransaction&) = delete;
		auto operator=(const FMultiAssetImportTransaction&) -> FMultiAssetImportTransaction& = delete;
		FMultiAssetImportTransaction(FMultiAssetImportTransaction&&) = delete;
		auto operator=(FMultiAssetImportTransaction&&) -> FMultiAssetImportTransaction& = delete;

		auto AddTexture(FPortableTextureBuildRequest Request) -> void;
		auto AddPackage(DPackage* Package, bool bRootPackage = false) -> void;
		auto AddLoadedObjectMutation(
			std::function<bool(std::string&)> Apply,
			std::function<void()> Rollback) -> void;
		auto SetFailurePoint(EImportTransactionFailurePoint Point, size_t Occurrence = 0) -> void;

		auto Prepare(std::string& OutError) -> bool;
		auto Stage(std::string& OutError) -> bool;
		auto Publish(std::string& OutError) -> bool;
		auto Rollback() -> void;
		auto Execute() -> FImportTransactionResult;
		auto GetTextures() const -> std::span<DTexture2D* const>;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
