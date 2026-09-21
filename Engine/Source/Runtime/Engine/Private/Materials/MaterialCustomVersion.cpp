#include "Materials/MaterialCustomVersion.h"
#include "Serialization/Archive.h"

namespace Durin
{
	namespace
	{
		const FCustomVersionRegistration OutputRegistration{
			FMaterialOutputVersion::Guid, FMaterialOutputVersion::CurrentVersion, "MaterialOutputNode"};
		const FCustomVersionRegistration GraphRegistration{
			FMaterialGraphVersion::Guid, FMaterialGraphVersion::CurrentVersion, "MaterialGraph"};
		const FCustomVersionRegistration FunctionRegistration{
			FMaterialFunctionVersion::Guid, FMaterialFunctionVersion::CurrentVersion, "MaterialFunctionPorts"};
		const FCustomVersionRegistration InstanceRegistration{
			FMaterialInstanceVersion::Guid, FMaterialInstanceVersion::CurrentVersion, "MaterialInstanceParameters"};

		auto SerializePackageVersion(FArchive& Ar, FGuid Guid, int32 CurrentVersion, std::string_view Name) -> bool
		{
			const auto Purpose = Ar.GetPurpose();
			if (Purpose != EArchivePurpose::Discovery && Purpose != EArchivePurpose::AuthoredPackage
				&& Purpose != EArchivePurpose::CookedPackage) return !Ar.IsError();
			Ar.UsingCustomVersion(Guid);
			if (Ar.IsLoading())
			{
				const auto* Version = Ar.GetVersionContext().FindCustom(Guid);
				if (!Version || Version->Version != CurrentVersion)
					Ar.Fail(EArchiveFailureCode::UnsupportedVersion, std::format(
						"{} requires custom version {} at {}; file version is {}.", Name,
						Guid.ToString(), CurrentVersion, Version ? std::to_string(Version->Version) : "missing"));
			}
			return !Ar.IsError();
		}
	}

	auto FMaterialOutputVersion::Serialize(FArchive& Ar) -> bool
	{
		return SerializePackageVersion(Ar, Guid, CurrentVersion, "MaterialOutputNode");
	}

	auto FMaterialGraphVersion::Serialize(FArchive& Ar) -> bool
	{
		return SerializePackageVersion(Ar, Guid, CurrentVersion, "MaterialGraph");
	}

	auto FMaterialFunctionVersion::Serialize(FArchive& Ar) -> bool
	{
		return SerializePackageVersion(Ar, Guid, CurrentVersion, "MaterialFunctionPorts");
	}

	auto FMaterialInstanceVersion::Serialize(FArchive& Ar) -> bool
	{
		return SerializePackageVersion(Ar, Guid, CurrentVersion, "MaterialInstanceParameters");
	}
}
