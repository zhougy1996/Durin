#include "AssetPackageLinker.h"

#include "Asset/Redirector.h"
#include "DObject/Package.h"
#include "DObject/PackagePersistence.h"

namespace Durin::AssetPrivate
{
 auto CaptureLivePackageLinker(DPackage* Package, EDefaultDeltaMode DeltaMode,
  const FAssetPackageSerializationOptions& Options, ObjectPackage::FLinkerTables& OutLinker,
  uint32 FormatVersion) -> FPackageCaptureResult
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
     return {.Error = {.Reason = EPackageCaptureReason::AssetIdentity, .Message = "Redirector destination is invalid."}};
    Capture.RedirectDestinations.emplace(Asset, std::move(Path));
   }
  return SaveContext.Capture(Package, OutLinker, FormatVersion);
 }
}
