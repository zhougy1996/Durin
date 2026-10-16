#include "Asset/CookBuildProviders.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Serialization/BinaryFormat.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/ITextureCompressorModule.h"
#include "TextureDerivedDataKey.h"
#include "DerivedDataBuildDefinition.h"
#endif

namespace Durin::AssetPrivate
{
	namespace
	{
		auto DeclareTextureInputs(const FCookDependencyRequest& Request, std::vector<FCookDependencyDeclaration>& Out,
			std::string_view Family, uint32 FamilyIndex) -> FCookContributionResult
		{
#if DURIN_WITH_EDITORONLY_DATA
			const auto* Module = ITextureCompressorModule::Get();
			const auto Kind = FamilyIndex == 0 ? ETextureBuildKind::Texture2D
				: FamilyIndex == 1 ? ETextureBuildKind::TextureCube : ETextureBuildKind::VolumeTexture;
			const auto Version = Module ? Module->GetBuildVersion(Kind) : 0;
			if (Version)
			{
				const auto Descriptor = GetTextureBuildDescriptor(Kind);
				FBinaryWriter Recipe;
				Recipe.WriteU64(Version);
				Recipe.WriteU32(Descriptor.Version);
				Recipe.WriteU32(Descriptor.ConstantsSchema);
				Recipe.WriteU32(Descriptor.OutputSchema);
				Recipe.WriteU32(TexturePayloadSchemaVersion);
				Out.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "recipe/" + std::string(Family), {}, Recipe.TakeBytes()});
				return {};
			}
#endif
			return {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::RecipeProvider, .VirtualPath = Request.Package.ToString(),
				.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile, .Provider = std::string(Family)};
		}
	}
	auto DeclareTexture2DCookInputs(const FCookDependencyRequest& Request, std::vector<FCookDependencyDeclaration>& Out) -> FCookContributionResult
	{ return DeclareTextureInputs(Request, Out, "texture2d", 0); }
	auto DeclareTextureCubeCookInputs(const FCookDependencyRequest& Request, std::vector<FCookDependencyDeclaration>& Out) -> FCookContributionResult
	{ return DeclareTextureInputs(Request, Out, "texture-cube", 1); }
	auto DeclareVolumeTextureCookInputs(const FCookDependencyRequest& Request, std::vector<FCookDependencyDeclaration>& Out) -> FCookContributionResult
	{ return DeclareTextureInputs(Request, Out, "volume-texture", 2); }
}

namespace Durin
{
	auto DTexture2D::AppendToClassSchema(FObjectCookContext& Context) -> void
	{ Context.VersionCoveredClasses.insert("Durin::DTexture2D"); AssetPrivate::AppendNativeCookSchema(Context, "texture2d", 2, AssetPrivate::DeclareTexture2DCookInputs); }
	auto DTextureCube::AppendToClassSchema(FObjectCookContext& Context) -> void
	{ Context.VersionCoveredClasses.insert("Durin::DTextureCube"); AssetPrivate::AppendNativeCookSchema(Context, "texture-cube", 2, AssetPrivate::DeclareTextureCubeCookInputs); }
	auto DVolumeTexture::AppendToClassSchema(FObjectCookContext& Context) -> void
	{ Context.VersionCoveredClasses.insert("Durin::DVolumeTexture"); AssetPrivate::AppendNativeCookSchema(Context, "volume-texture", 2, AssetPrivate::DeclareVolumeTextureCookInputs); }
}
