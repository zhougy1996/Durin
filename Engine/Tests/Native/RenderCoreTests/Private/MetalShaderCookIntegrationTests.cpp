#include "ShaderCookIntegrationTestSupport.h"

namespace Durin
{
	TEST(FMetalShaderCookIntegrationTests, ProducesAndLoadsCompleteGameLibraryWithoutProducer)
	{
		QualifyCompleteGameLibrary(true);
	}
}
