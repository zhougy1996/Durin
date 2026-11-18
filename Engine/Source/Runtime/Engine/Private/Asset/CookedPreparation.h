#pragma once
#include "CoreMinimal.h"
#include "Asset/Cook.h"
#include "DObject/PackageCapture.h"

namespace Durin::AssetPrivate
{
	enum class EPollStatus : uint8 { Success, Error, Incomplete };
	class ENGINE_API FPackageCookPreparation
	{
	public:
		FPackageCookPreparation() = default;
		~FPackageCookPreparation();
		FPackageCookPreparation(const FPackageCookPreparation&) = delete;
		auto operator=(const FPackageCookPreparation&) -> FPackageCookPreparation& = delete;
		auto Begin(DPackage* Package, const FCookPackagePreparationOptions& Options,
			const FAssetPackageSerializationOptions& Serialization, FCookPackagePreparationReport& Report) -> FObjectCookResult;
		auto Poll() -> EPollStatus;
		auto Cancel() -> void;
		auto Validate(DPackage* Package) const -> FObjectCookResult;
		auto GetResult() const -> const FObjectCookResult& { return Result; }
		auto GetTargetPlatform() const -> const ITargetPlatform* { return Platform; }
		auto GetArchiveTarget() const -> const FArchiveTarget& { return Target; }
	private:
		DPackage* Package = nullptr;
		FCookPackagePreparationOptions Options;
		FAssetPackageSerializationOptions Serialization;
		FCookPackagePreparationReport* Report = nullptr;
		uint64 Revision = 0;
		std::vector<DObject*> Objects;
		std::vector<std::shared_ptr<FCookedPlatformData>> Operations;
		FArchiveTarget Target;
		const ITargetPlatform* Platform = nullptr;
		FObjectCookResult Result;
		bool bComplete = false;
	};
}
