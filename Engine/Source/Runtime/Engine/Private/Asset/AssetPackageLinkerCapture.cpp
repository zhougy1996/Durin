#include "AssetPackageLinker.h"

#include "Asset/Redirector.h"
#include "DObject/Package.h"
#include "DObject/PackagePersistence.h"

namespace Durin::AssetPrivate
{
 auto CaptureLivePackageLinker(DPackage* Package, EDefaultDeltaMode DeltaMode,
  const FAssetPackageSerializationOptions& Options, ObjectPackage::FLinkerTables& OutLinker,
  uint32 FormatVersion) -> FAssetResult
 {
  FSavePackageContext SaveContext;
  SaveContext.Options.Mode = DeltaMode == EDefaultDeltaMode::NoDelta ? EPackageSaveMode::Complete : EPackageSaveMode::Delta;
  auto& Capture = SaveContext.Options.Capture;
  Capture.bCooking = Options.Domain == EAssetPackageSaveDomain::Cooked;
  Capture.bRetainEditorOnlyData = Options.bRetainEditorOnlyData;
  Capture.Target.Platform = Options.TargetPlatform == ECookTargetPlatform::Win64 ? "Win64" : "";
  Capture.Target.Profile = Options.TargetProfile == ECookTargetProfile::Game ? "Game"
   : Options.TargetProfile == ECookTargetProfile::EditorValidation ? "EditorValidation" : "";
  Capture.SaveOverrides = Options.SaveOverrides;
  Capture.PropertyFilter = Options.PropertyFilter;
  if (Package) for (DObject* Asset : Package->GetTopLevelAssets())
   if (auto* Redirector = Cast<DAssetRedirector>(Asset))
   {
    DObject* Destination = Redirector->GetDestinationObject();
    FObjectPath Path;
    if (!Destination || Destination == Asset || !Destination->GetPackage()
     || !FObjectPath::TryCreate(Destination->GetObjectPath(), Path))
     return {EAssetError::CorruptFile, "Redirector destination is invalid."};
    Capture.RedirectDestinations.emplace(Asset, std::move(Path));
   }
  auto Captured = SaveContext.Capture(Package, OutLinker, FormatVersion);
  if (Captured) return {};
  // Explicit adapter to the still-unmigrated outer asset result contract.
  FAssetResult Result;
  Result.Message = FormatPackageCaptureError(Captured.Error);
  switch (GetPackageCaptureSaveError(Captured.Error))
  {
   case EPackageSaveError::InvalidPath: Result.Error = EAssetError::InvalidPath; break;
   case EPackageSaveError::InvalidPackageType: Result.Error = EAssetError::InvalidPackageType; break;
   case EPackageSaveError::InvalidObjectGraph: Result.Error = EAssetError::InvalidObjectGraph; break;
   case EPackageSaveError::UnsupportedVersion: Result.Error = EAssetError::UnsupportedVersion; break;
   default: Result.Error = EAssetError::UnsupportedProperty; break;
  }
  Result.PackageCaptureCause = std::make_shared<FPackageCaptureError>(std::move(Captured.Error));
  return Result;
 }
}
