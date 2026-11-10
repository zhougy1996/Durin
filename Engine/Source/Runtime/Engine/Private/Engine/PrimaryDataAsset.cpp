#include "Engine/PrimaryDataAsset.h"
#include "DObject/Class.h"

namespace Durin
{
	DPrimaryDataAsset::DPrimaryDataAsset(const FObjectInitializer& Initializer) : Super(Initializer) {}
	auto DPrimaryDataAsset::GetPrimaryAssetId() const -> FPrimaryAssetId
	{
		auto NativeName = GetClass()->GetQualifiedName().ToString();
		if (const auto Scope = NativeName.rfind("::"); Scope != std::string::npos) NativeName.erase(0, Scope + 2);
		return {FPrimaryAssetType(FName(NativeName)), GetFName()};
	}
	auto DPrimaryDataAsset::GetAssetBundleData() const -> FAssetBundleData
	{
		FAssetBundleData Data;
		UpdateAssetBundleData(Data);
		return Data;
	}
	auto DPrimaryDataAsset::UpdateAssetBundleData(FAssetBundleData&) const -> void {}
}
