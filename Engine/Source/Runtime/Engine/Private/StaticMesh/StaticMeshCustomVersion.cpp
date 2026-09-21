#include "StaticMesh/StaticMeshCustomVersion.h"
#include "Serialization/Archive.h"

namespace Durin
{
	namespace
	{
		const FCustomVersionRegistration SourceRegistration{
			FStaticMeshSourceVersion::Guid, FStaticMeshSourceVersion::CurrentVersion, "StaticMeshSource"};
	}

	auto FStaticMeshSourceVersion::Serialize(FArchive& Ar) -> bool
	{
		if (Ar.IsFilterEditorOnly() || (Ar.GetPurpose() != EArchivePurpose::Discovery
			&& Ar.GetPurpose() != EArchivePurpose::AuthoredPackage)) return !Ar.IsError();
		Ar.UsingCustomVersion(Guid);
		if (Ar.IsLoading())
		{
			const auto* Version = Ar.GetVersionContext().FindCustom(Guid);
			if (!Version || Version->Version != CurrentVersion)
				Ar.Fail(EArchiveFailureCode::UnsupportedVersion, std::format(
					"StaticMeshSource requires custom version {} at {}; file version is {}.",
					Guid.ToString(), CurrentVersion, Version ? std::to_string(Version->Version) : "missing"));
		}
		return !Ar.IsError();
	}
}
