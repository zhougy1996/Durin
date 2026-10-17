#include "SceneRenderQualification.h"

namespace Durin
{
	TEST(FVolumetricCloudSceneVulkanTests, EnabledCloudTraversesOffscreenAndPresentRoutes)
	{
		QualifySceneRenderRoutes(true);
	}
}
