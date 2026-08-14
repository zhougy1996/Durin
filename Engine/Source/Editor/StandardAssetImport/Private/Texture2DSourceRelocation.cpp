#include "Texture2DSourceRelocation.h"

#include "DObject/Object.h"
#include "Source/MountedSourceRelocation.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DAuthoringService.h"
#include "Texture2DSourceTranslation.h"
#include "TextureCubeSourceTranslation.h"

namespace Durin::Asset::Import::Standard
{
	namespace
	{
		Editor::FMountedSourceRelocationHandlerHandle GTexture2DRelocationHandler = 0;
		Editor::FMountedSourceRelocationHandlerHandle GTextureCubeRelocationHandler = 0;
	}

	auto RegisterTexture2DSourceRelocation() -> bool
	{
		if (GTexture2DRelocationHandler != 0) return true;
		GTexture2DRelocationHandler = Editor::RegisterMountedSourceRelocationHandler(
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
				if (!ChangeTexture2DSourceReference(*Texture, To, OutError)) return false;
				if (Asset::Build::WaitForTexture2DBuild(*Texture)) return true;
				const std::string BuildError =
					Asset::Build::GetTexture2DBuildDiagnostic(*Texture).Message;
				OutError = BuildError.empty()
					? "Texture2D source relocation build did not complete."
					: BuildError;
				return false;
			});
		if (GTexture2DRelocationHandler == 0) return false;
		GTextureCubeRelocationHandler = Editor::RegisterMountedSourceRelocationHandler(
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
					return ChangeTextureCubePanoramaSourceReference(*Cube, To, {
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
				return ChangeTextureCubeFaceSourceReferences(
					*Cube, Paths, {.bSRGB = Cube->IsSRGB()}, OutError);
			});
		if (GTextureCubeRelocationHandler != 0) return true;
		Editor::UnregisterMountedSourceRelocationHandler(GTexture2DRelocationHandler);
		GTexture2DRelocationHandler = 0;
		return false;
	}

	auto UnregisterTexture2DSourceRelocation() -> void
	{
		Editor::UnregisterMountedSourceRelocationHandler(GTextureCubeRelocationHandler);
		GTextureCubeRelocationHandler = 0;
		Editor::UnregisterMountedSourceRelocationHandler(GTexture2DRelocationHandler);
		GTexture2DRelocationHandler = 0;
	}
}
