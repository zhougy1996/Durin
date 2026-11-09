#include "AssetPackageLinker.h"

#include "Asset/Redirector.h"
#include "DObject/Package.h"
#include "DObject/PackagePersistence.h"

namespace Durin::AssetPrivate
{
 auto CaptureLivePackageLinker(DPackage* Package, EDefaultDeltaMode DeltaMode,
  const FAssetPackageCaptureOptions& Options, ObjectPackage::FLinkerTables& OutLinker,
  uint32 FormatVersion) -> std::expected<void, FPackageCaptureError>
 {
  FSavePackageContext SaveContext;
  SaveContext.Options.Mode = DeltaMode == EDefaultDeltaMode::NoDelta ? EPackageSaveMode::Complete : EPackageSaveMode::Delta;
  auto& Capture = SaveContext.Options.Capture;
  Capture.bCooking = Options.CookTarget.has_value();
  Capture.bRetainEditorOnlyData = Options.Serialization.bRetainEditorOnlyData;
  if (Options.CookTarget) Capture.Target = *Options.CookTarget;
  Capture.SaveOverrides = Options.Serialization.SaveOverrides;
  Capture.PropertyFilter = Options.Serialization.PropertyFilter;
  if (Capture.bCooking) Capture.ObjectFilter = [Target = Capture.Target](const DObject* Object) {
   return IsObjectIncludedForCook(Object, Target);
  };
  if (Package) for (DObject* Asset : Package->GetTopLevelAssets())
   if (auto* Redirector = Cast<DAssetRedirector>(Asset))
   {
    DObject* Destination = Redirector->GetDestinationObject();
    FObjectPath Path;
    if (!Destination || Destination == Asset || !Destination->GetPackage()
     || !FObjectPath::TryCreate(Destination->GetObjectPath(), Path))
     return std::unexpected(FPackageCaptureError{.Reason = EPackageCaptureReason::AssetIdentity, .Message = "Redirector destination is invalid."});
    Capture.RedirectDestinations.emplace(Asset, std::move(Path));
   }
  if (auto Result = SaveContext.Capture(Package, OutLinker, FormatVersion); !Result) return Result;
  return {};
 }
}
