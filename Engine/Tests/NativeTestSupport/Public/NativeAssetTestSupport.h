#pragma once

#include "Asset/Asset.h"
#include "Asset/Redirector.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"

namespace Durin::Testing
{
	inline auto CreateAssetRedirectorForTests(
		const FPackagePath& RedirectorPath,
		const FPackagePath& DestinationPath,
		DAssetRedirector*& OutRedirector) -> FAssetResult
	{
		OutRedirector = nullptr;
		if (!RedirectorPath.IsValid() || !DestinationPath.IsValid()
			|| RedirectorPath == DestinationPath)
			return {EAssetError::InvalidPath,
				"Redirector source and destination paths must be valid and distinct."};

		const FAssetPathResolveResult Resolution = ResolveAssetPath(DestinationPath);
		if (!Resolution)
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::NotFound:
			case EAssetPathResolveState::MissingRedirectTarget:
				return {EAssetError::NotFound,
					"Redirector destination does not resolve to a registered asset."};
			case EAssetPathResolveState::RedirectCycle:
			case EAssetPathResolveState::RedirectDepthExceeded:
				return {EAssetError::CircularDependency,
					"Redirector destination does not have a finite canonical target."};
			case EAssetPathResolveState::UnknownTargetClass:
				return {EAssetError::UnknownClass,
					"Redirector destination has an unavailable reflected class."};
			case EAssetPathResolveState::RedirectTypeMismatch:
				return {EAssetError::TypeMismatch,
					"Redirector destination has an incompatible asset class."};
			case EAssetPathResolveState::CorruptRedirector:
				return {EAssetError::CorruptFile,
					"Redirector destination traverses corrupt redirect metadata."};
			case EAssetPathResolveState::Resolved:
				break;
			}
		}
		if (!Resolution.FinalAssetData)
			return {EAssetError::InvalidObjectGraph,
				"The redirect destination has no final asset data."};
		const FAssetData& DestinationData = *Resolution.FinalAssetData;
		const auto DestinationRecord = std::ranges::find(
			DestinationData.TopLevelAssets, DestinationData.AssetClassName,
			&FTopLevelAssetData::AssetClassName);
		if (DestinationRecord == DestinationData.TopLevelAssets.end())
			return {EAssetError::InvalidObjectGraph,
				"The redirect destination has no exact top-level asset."};

		FObjectPath DestinationObjectPath;
		if (!FObjectPath::TryCreate(
			DestinationRecord->AssetPath, std::span<const std::string>{},
			DestinationObjectPath))
			return {EAssetError::InvalidPath,
				"The redirect destination object path is invalid."};
		DObject* DestinationObject = nullptr;
		FAssetResult Result = LoadObject(
			DestinationObjectPath, nullptr, DestinationObject);
		if (!Result) return Result;

		// Build serialization fixtures through reflection without adding a runtime test API.
		FProperty* DestinationProperty =
			DAssetRedirector::StaticClass()->FindPropertyByName("DestinationObject");
		if (!DestinationProperty
			|| DestinationProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
			return {EAssetError::InvalidObjectGraph,
				"The redirector fixture requires a reflected destination object property."};

		DPackage* Package = CreatePackage(RedirectorPath);
		if (!Package)
			return {EAssetError::AlreadyExists,
				"The redirector fixture package could not be created."};
		FStaticConstructObjectParameters Parameters{
			DAssetRedirector::StaticClass(), Package,
			FName(RedirectorPath.GetPackageName()), sizeof(DAssetRedirector),
			EObjectFlags::Public};
		DObject* Object = StaticConstructObject(Parameters);
		DObjectForceRegistration(Object);
		OutRedirector = Cast<DAssetRedirector>(Object);
		if (!OutRedirector
			|| Package->FindTopLevelAsset(OutRedirector->GetFName()) != OutRedirector)
		{
			MarkObjectHierarchyAsGarbage(Package);
			CollectGarbage();
			OutRedirector = nullptr;
			return {EAssetError::InvalidObjectGraph,
				"The redirector fixture could not be registered as a top-level asset."};
		}
		static_cast<FObjectProperty*>(DestinationProperty)->SetObjectPropertyValue(
			OutRedirector, DestinationObject);
		Package->MarkDirty();
		Package->MarkAsNewlyCreated();
		return {};
	}
}
