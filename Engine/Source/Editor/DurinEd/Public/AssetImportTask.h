#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "AssetImportTask.gen.h"

namespace Durin
{
	class DFactory;
	class FAssetTools;
	class IAssetImportTaskOperation;
	struct FAssetImportTaskAccess;

	// Owner-thread inputs and managed outputs for one source file. Ordinary factories
	// complete synchronously even when bAsync is requested.
	DCLASS()
	class DAssetImportTask : public DObject
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DAssetImportTask(const FObjectInitializer& Initializer);
		DPROPERTY(Transient)
		std::string Filename;
		DPROPERTY(Transient)
		std::string DestinationPath;
		DPROPERTY(Transient)
		std::string DestinationName;
		DPROPERTY(Transient)
		bool bReplaceExisting = false;
		DPROPERTY(Transient)
		bool bReplaceExistingSettings = false;
		DPROPERTY(Transient)
		bool bAutomated = false;
		DPROPERTY(Transient)
		bool bSave = false;
		DPROPERTY(Transient)
		bool bAsync = false;
		DPROPERTY(Transient)
		TObjectPtr<DFactory> Factory;
		DPROPERTY(Transient)
		TObjectPtr<DObject> Options;
		DPROPERTY(Transient)
		std::vector<std::string> ImportedObjectPaths;

		DURINED_API auto GetObjects() const -> const std::vector<DObject*>&;
		DURINED_API auto IsAsyncImportComplete() const -> bool;
	private:
		friend class FAssetTools;
		friend struct FAssetImportTaskAccess;
		bool bRunning = false;
		bool bAdvancing = false;
		std::function<void()> Completion;
		std::shared_ptr<IAssetImportTaskOperation> Operation;
		DPROPERTY(Transient)
		std::vector<TObjectPtr<DObject>> Results;
		mutable std::vector<DObject*> ObjectView;
	};
}
