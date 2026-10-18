#include "ShaderCookIntegrationTestSupport.h"

namespace Durin
{
	TEST(FShaderCookIntegrationTests, ProducesDeterministicCompleteGameLibraryThroughModule)
	{
		QualifyCompleteGameLibrary(false);
	}
}
