#include "ObjectTools.h"
#include "AssetTools/IAssetTools.h"
#include "Import/AssetDestinationValidation.h"
#include "Asset/Load.h"
#include "Asset/Redirector.h"
#include "DObject/Archive.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/FeedbackContext.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto SanitizeName(std::string Name) -> std::string
		{
			for (char& C : Name) if (std::string_view("\\/:*?\"<>| .\t\r\n").find(C) != std::string_view::npos) C = '_';
			return Name;
		}
		auto Reject(std::string_view Message) -> DObject*
		{ FFeedbackContext::Get().Error(Message); return nullptr; }
	}

	auto MakeUniqueAssetName(std::string_view Base, std::string_view Suffix, std::string& Package, std::string& Name) -> void
	{
		check(IsInGameThread());
		Package.clear(); Name.clear();
		const auto Slash = Base.find_last_of('/');
		if (Slash == std::string_view::npos) { Reject("Unique naming requires a logical package path."); return; }
		const std::string Directory(Base.substr(0, Slash + 1));
		const std::string Original = SanitizeName(std::string(Base.substr(Slash + 1)) + std::string(Suffix));
		if (Original.empty()) { Reject("Unique naming requires an asset name."); return; }
		size_t Digits = Original.size();
		while (Digits && Original[Digits - 1] >= '0' && Original[Digits - 1] <= '9') --Digits;
		const size_t Width = Original.size() - Digits;
		std::string Stem = Original.substr(0, Digits);
		uint64 Number = 0;
		if (!Digits) { Stem = Original + "_"; Number = 2; }
		else for (size_t Index = Digits; Index < Original.size(); ++Index)
		{
			if (Number > (std::numeric_limits<uint64>::max() - 9) / 10) { Reject("The numeric name suffix is too large."); return; }
			Number = Number * 10 + Original[Index] - '0';
		}
		for (;;)
		{
			std::string Candidate = Original;
			if (Number)
			{
				auto Text = std::to_string(Number);
				if (Digits && Text.size() < Width) Text.insert(0, Width - Text.size(), '0');
				Candidate = Stem + Text;
			}
			const auto Destination = InspectAssetDestination(Directory + Candidate);
			if (!Destination && !Destination.AssetExists()) { Reject(FormatAssetDestinationValidation(Destination)); return; }
			std::error_code Error;
			const bool Exists = std::filesystem::exists(Destination.PhysicalPath, Error);
			if (Error) { Reject("Could not inspect the duplicate destination: " + Error.message()); return; }
			if (!Destination.AssetExists() && !Exists)
			{ Package = Directory + Candidate; Name = Candidate; return; }
			if (Number == std::numeric_limits<uint64>::max()) { Reject("The numeric name suffix is exhausted."); return; }
			++Number;
		}
	}

	auto DuplicateAssetWithEditorPolicy(std::string_view Name, std::string_view Directory, DObject* Source) -> DObject*
	{
		check(IsInGameThread());
		if (!Source || !Source->GetPackage() || Source->GetOuter() != Source->GetPackage()
			|| Source->GetPackage()->FindTopLevelAsset(Source->GetFName()) != Source) return Reject("Duplication requires a live top-level asset.");
		if (Cast<DAssetRedirector>(Source)) return Reject("Redirectors cannot be duplicated as authored assets.");
		if (Source->GetPackage()->IsGraphPrivate() || IsPackageLoading(Source->GetPackage()->GetPackagePathIdentity()))
			return Reject("Duplication requires a published, fully loaded asset.");
		const TStrongObjectPtr<DObject> SourceOwner(Source);
		std::string Prefix(Directory);
		if (!Prefix.ends_with('/')) Prefix += '/';
		FPackagePath Path;
		FTopLevelAssetPath Target;
		if (!FPackagePath::TryCreate(Prefix + std::string(Name), Path) || !FTopLevelAssetPath::TryCreate(Path, Name, Target)) return Reject("The duplicate identity is invalid.");
		const auto Destination = InspectAssetDestination(Path.GetView());
		std::error_code Error;
		if (!Destination || Destination.AssetExists() || std::filesystem::exists(Destination.PhysicalPath, Error) || Error) return Reject("The duplicate destination is occupied or unavailable.");
		auto* Package = CreatePackage(Path);
		if (!Package) return Reject("The duplicate package could not be created.");
		const TStrongObjectPtr<DPackage> Owner(Package);
		const auto Result = DuplicateObject(Source, Package, FName(Name));
		if (!Result)
		{
			MarkObjectHierarchyAsGarbage(Package);
			Package->RetireFailedLoadRegistration();
			RequestGarbageCollection();
			return Reject(ToString(Result.error()));
		}
		(*Result)->SetFlags(EObjectFlags::Public | EObjectFlags::Standalone | EObjectFlags::Transactional);
		Package->MarkDirty(); Package->MarkAsNewlyCreated();
		return *Result;
	}

	namespace ObjectTools
	{
		namespace { FDuplicatePrompt DuplicatePrompt; }
		auto SetDuplicatePrompt(FDuplicatePrompt Prompt) -> void
		{ check(IsInGameThread()); DuplicatePrompt = std::move(Prompt); }
		auto DuplicateObjects(std::span<DObject* const> Objects, std::string_view SourcePath,
			std::string_view Destination, bool OpenDialog, std::vector<DObject*>* Output) -> void
		{
			check(IsInGameThread());
			std::vector<TStrongObjectPtr<DObject>> Owners;
			for (auto* Object : Objects) Owners.emplace_back(Object);
			std::string Target(Destination);
			while (Target.ends_with('/')) Target.pop_back();
			if (OpenDialog)
			{
				const auto Prompt = DuplicatePrompt;
				if (!Prompt) { Reject("Interactive duplicate has no destination prompt host."); return; }
				const auto Chosen = Prompt(Destination);
				if (!Chosen) return;
				Target = *Chosen;
				while (Target.ends_with('/')) Target.pop_back();
			}
			for (const auto& Owner : Owners)
			{
				auto* Object = Owner.Get();
				if (!Object || !Object->GetPackage()) continue;
				const auto Source = Object->GetPackage()->GetPackagePath();
				std::string Directory = Target.empty() ? Source.substr(0, Source.find_last_of('/')) : Target;
				if (!SourcePath.empty() && Source.starts_with(std::string(SourcePath) + "/"))
				{
					const auto Relative = Source.substr(SourcePath.size(), Source.find_last_of('/') - SourcePath.size());
					Directory += Relative;
				}
				std::string Package, Name;
				MakeUniqueAssetName(Directory + "/" + Object->GetName(), {}, Package, Name);
				if (Name.empty()) continue;
				if (auto* Copy = IAssetTools::Get().DuplicateAsset(Name, Directory, Object); Copy && Output) Output->push_back(Copy);
			}
		}
	}
}
