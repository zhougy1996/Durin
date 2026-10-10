#include "ShaderBuildVersion.h"
#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ShaderBuildPrivate
{
	auto BuildVersion(std::string_view CompilerEnvironment) -> uint64
	{
		FBinaryWriter Writer;
		Writer.WriteString("Durin.ShaderBuild.Compile");
		Writer.WriteU32(CompileAlgorithmRevision);
		Writer.WriteU32(ReflectionAlgorithmRevision);
#if defined(__APPLE__)
		Writer.WriteU32(MetalTranslationAlgorithmRevision);
#endif
		Writer.WriteString(CompilerEnvironment);
		const uint64 Hash = FXxHash64::HashBuffer(Writer.TakeBytes()).HashValue;
		return Hash ? Hash : uint64{1};
	}
}
