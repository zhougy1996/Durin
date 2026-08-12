#include "Texture2DSourceRelocation.h"

#include "DObject/Object.h"
#include "Source/MountedSourceRelocation.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DAuthoringService.h"
#include "Texture2DSourceTranslation.h"

namespace Durin::StandardAssetImport
{
	namespace
	{
		Editor::FMountedSourceRelocationHandlerHandle GTexture2DRelocationHandler = 0;
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
				if (AssetBuild::WaitForTexture2DBuild(*Texture)) return true;
				const std::string BuildError =
					AssetBuild::GetTexture2DBuildDiagnostic(*Texture).Message;
				OutError = BuildError.empty()
					? "Texture2D source relocation build did not complete."
					: BuildError;
				return false;
			});
		return GTexture2DRelocationHandler != 0;
	}

	auto UnregisterTexture2DSourceRelocation() -> void
	{
		Editor::UnregisterMountedSourceRelocationHandler(GTexture2DRelocationHandler);
		GTexture2DRelocationHandler = 0;
	}
}
