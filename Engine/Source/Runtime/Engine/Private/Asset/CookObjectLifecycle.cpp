#include "Asset/Cook.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Package.h"

#if DURIN_WITH_EDITOR
#include "TargetPlatform.h"
#endif

namespace Durin
{
	auto FCookedPlatformData::GetTargetPlatform() const -> ECookTargetPlatform {
#if DURIN_WITH_EDITOR
		if (Platform) return Platform->GetPlatform();
#endif
		return ECookTargetPlatform::Invalid;
	}
	auto FCookedPlatformData::GetArchiveTarget() const -> FArchiveTarget
	{ return {std::string(GetTargetPlatformName(GetTargetPlatform())), std::string(GetTargetProfileName(GetTargetProfile()))}; }
	auto FCookedPlatformData::GetTargetProfile() const -> ECookTargetProfile { return Options.TargetProfile; }
	auto FCookedPlatformData::ReadDeclaredInput(ECookBuildDependencyKind Kind, std::string_view Name, FByteBuffer& Out) const -> FCookInputResult
	{
		Out.clear();
		return Options.ReadCookInput ? Options.ReadCookInput(Kind, Name, Out)
			: FCookInputFailure{.Error = ECookInputError::NoReader, .Kind = Kind, .Name = std::string(Name)}.ToInputResult();
	}
}
