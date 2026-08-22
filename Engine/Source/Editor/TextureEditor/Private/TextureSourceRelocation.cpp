#include "TextureSourceRelocation.h"

#include "DObject/Object.h"
#include "Source/MountedSourceRelocation.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DAuthoringService.h"
#include "Texture2DSourceTranslation.h"
#include "TextureCubeSourceTranslation.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		FMountedSourceRelocationHandlerHandle GTexture2DRelocationHandler = 0;
		FMountedSourceRelocationHandlerHandle GTextureCubeRelocationHandler = 0;
	}

	auto RegisterTextureSourceRelocation() -> bool
	{
		if (GTexture2DRelocationHandler != 0) return true;
		GTexture2DRelocationHandler = RegisterMountedSourceRelocationHandler(
			[](DObject& Asset,
				std::string_view From,
				std::string_view To,
				std::string& OutError) -> std::optional<bool> {
				DTexture2D* Texture = Cast<DTexture2D>(&Asset);
				if (!Texture) return std::nullopt;
				if (Texture->GetSourceFile() != From)
				{
					OutError = "Texture2D no longer references the source being relocated.";
					return false;
				}
				if (!Asset::Forge::ChangeTexture2DSourceReference(*Texture, To, OutError))
					return false;
				if (Asset::Build::WaitForTexture2DBuild(*Texture)) return true;
				const std::string BuildError =
					Asset::Build::GetTexture2DBuildDiagnostic(*Texture).Message;
				OutError = BuildError.empty()
					? "Texture2D source relocation build did not complete."
					: BuildError;
				return false;
			});
		if (GTexture2DRelocationHandler == 0) return false;
		GTextureCubeRelocationHandler = RegisterMountedSourceRelocationHandler(
			[](DObject& Asset,
				std::string_view From,
				std::string_view To,
				std::string& OutError) -> std::optional<bool> {
				auto* Cube = Cast<DTextureCube>(&Asset);
				if (!Cube) return std::nullopt;
				if (Cube->GetSourceLayout()
					== ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					if (Cube->GetPanoramaSourceFile() != From)
					{
						OutError = "TextureCube panorama no longer references the source being relocated.";
						return false;
					}
					return Asset::Forge::ChangeTextureCubePanoramaSourceReference(
						*Cube, To, {
							.FaceDimension = Cube->GetPanoramaFaceDimension(),
							.ExposureEV = Cube->GetPanoramaExposureEV()}, OutError);
				}
				std::array<std::string, TextureCubeFaceCount> Paths;
				bool bFound = false;
				for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
				{
					Paths[Index] = Cube->GetSourceFile(
						static_cast<ETextureCubeFace>(Index));
					if (Paths[Index] == From)
					{
						Paths[Index] = To;
						bFound = true;
					}
				}
				if (!bFound)
				{
					OutError = "TextureCube faces no longer reference the source being relocated.";
					return false;
				}
				return Asset::Forge::ChangeTextureCubeFaceSourceReferences(
					*Cube, Paths, {.bSRGB = Cube->IsSRGB()}, OutError);
			});
		if (GTextureCubeRelocationHandler != 0) return true;
		UnregisterMountedSourceRelocationHandler(GTexture2DRelocationHandler);
		GTexture2DRelocationHandler = 0;
		return false;
	}

	auto UnregisterTextureSourceRelocation() -> void
	{
		UnregisterMountedSourceRelocationHandler(GTextureCubeRelocationHandler);
		GTextureCubeRelocationHandler = 0;
		UnregisterMountedSourceRelocationHandler(GTexture2DRelocationHandler);
		GTexture2DRelocationHandler = 0;
	}
}
