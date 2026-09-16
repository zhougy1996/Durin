#pragma once

#include "AssetTools/AssetOperation.h"
#include "DObject/Object.h"

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

	struct FAssetImportValidation
	{
		const DFactory* Factory = nullptr;
		std::string Message;
		explicit operator bool() const { return Factory != nullptr && Message.empty(); }
	};

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
