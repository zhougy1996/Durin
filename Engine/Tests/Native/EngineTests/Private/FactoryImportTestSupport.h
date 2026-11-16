#pragma once

#include "CoreMinimal.h"
#include "AssetImportTask.h"
#include "DObject/ObjectLifecycle.h"
#include "AssetTools/IAssetTools.h"
#include "Factories/Factory.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/AssetPath.h"
#include "Misc/FeedbackContext.h"

namespace Durin::Testing
{
	inline auto ImportFactoryAssetForTest(const FPackagePath& Path, DClass* Class, std::string_view Filename,
		DFactory* Factory = nullptr, FFeedbackContext* Warn = nullptr) -> DObject*
	{
		FScopedFeedbackContext Scope(Warn ? *Warn : FFeedbackContext::Get());
		TStrongObjectPtr<DAssetImportTask> Task(NewObject<DAssetImportTask>(nullptr, "FactoryTestTask", EObjectFlags::Transient));
		Task->Filename = Filename; Task->DestinationName = Path.GetPackageName();
		const auto Package = Path.ToString(); Task->DestinationPath = Package.substr(0, Package.find_last_of('/'));
		if (!Factory)
		{
			const auto Candidates = DFactory::FindFactories(Class, std::filesystem::path(Filename).extension().generic_string());
			if (Candidates.size() == 1) Factory = Cast<DFactory>(NewObject(Candidates.front()->GetClass(), nullptr, "FactoryTestInvocation", EObjectFlags::Transient));
		}
		Task->Factory = Factory;
		IAssetTools::Get().ImportAssetTasks(std::array{Task.Get()});
		const auto& Objects = Task->GetObjects();
		auto* Object = Objects.empty() ? nullptr : Objects.front();
		MarkAsGarbage(Task.Get()); Task.Reset();
		return Object;
	}

	template <typename T>
	struct TFactoryImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		T* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};
}
