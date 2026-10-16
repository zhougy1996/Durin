#include "Asset/CookSchema.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Serialization/BinaryFormat.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/ITextureCompressorModule.h"
#include "TextureDerivedDataKey.h"
#include "DerivedDataBuildDefinition.h"
#endif

namespace Durin
{
	namespace
	{
		auto AppendTextureCookSchema(FObjectCookContext& Context, std::string_view Family, uint32 FamilyIndex) -> void
		{
			AssetPrivate::AppendCookSchemaVersion(Context, Family, 2);
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
				Context.Dependencies.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "recipe/" + std::string(Family), {}, Recipe.TakeBytes()});
				return;
			}
#endif
			Context.Result = {EObjectCookError::InvalidInput, std::format("Cook recipe provider '{}' is unavailable.", Family)};
		}
	}
	auto DTexture2D::AppendToClassSchema(FObjectCookContext& Context) -> void
	{ Context.VersionCoveredClasses.insert("Durin::DTexture2D"); AppendTextureCookSchema(Context, "texture2d", 0); }
	auto DTextureCube::AppendToClassSchema(FObjectCookContext& Context) -> void
	{ Context.VersionCoveredClasses.insert("Durin::DTextureCube"); AppendTextureCookSchema(Context, "texture-cube", 1); }
	auto DVolumeTexture::AppendToClassSchema(FObjectCookContext& Context) -> void
	{ Context.VersionCoveredClasses.insert("Durin::DVolumeTexture"); AppendTextureCookSchema(Context, "volume-texture", 2); }
}
