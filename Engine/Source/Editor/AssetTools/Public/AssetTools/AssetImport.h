#pragma once

#include "AssetTools/AssetOperation.h"
#include "DObject/Object.h"

namespace Durin::Editor { struct FAssetDestinationValidation; }

namespace Durin
{
	class DClass;
	class DFactory;

	// One standalone output. Factories own typed settings and multi-file layouts.
	struct FAssetImportRequest
	{
		FTopLevelAssetPath AssetPath;
		DClass* AssetClass = nullptr;
		std::string Filename;
		const DFactory* Factory = nullptr;
		DObject* Context = nullptr;
		EObjectFlags Flags = EObjectFlags::Public;
	};

	enum class EAssetImportError : uint8 { None, Path, Class, Filename, Destination, FileInspection, FileExists, AmbiguousFactory, MissingFactory, UnsupportedFactory, DuplicatePackage };
	struct FAssetImportValidation
	{
		const DFactory* Factory = nullptr;
		EAssetImportError Error = EAssetImportError::None;
		std::string AssetPath, Filename, ClassName, Extension;
		size_t Count = 0;
		std::error_code FileCause;
		std::shared_ptr<const Editor::FAssetDestinationValidation> DestinationCause;
		explicit operator bool() const { return Error == EAssetImportError::None; }
	};

	ASSETTOOLS_API auto FormatAssetImportValidation(const FAssetImportValidation& Result) -> std::string;

	enum class EAssetImportItemState : uint8
	{
		NotAttempted,
		Rejected,
		Accepted,
		Canceled,
	};

	struct FAssetImportItemResult
	{
		EAssetImportItemState State = EAssetImportItemState::NotAttempted;
		FAssetOperationResult Operation{
			.Kind = EAssetOperationKind::Import,
			.State = EAssetOperationTerminalState::Rejected};
	};

	struct FAssetImportBatchRequest
	{
		std::vector<FAssetImportRequest> Items;
		bool bStopOnFailure = false;
		// Called on the game thread before each item; cannot interrupt a factory.
		std::function<bool()> ShouldCancel;
	};

	struct FAssetImportBatchResult
	{
		// Input order, including rejected and unattempted items. Accepted means
		// factory acceptance, not completion of deferred family compilation/save.
		std::vector<FAssetImportItemResult> Items;
	};
}
