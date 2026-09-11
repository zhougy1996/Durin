#pragma once

#include "DObject/StrongObjectPtr.h"
#include "Materials/Material.h"

namespace Durin { class DTransactor; }

namespace Durin::Editor::Material
{
	// Owns a transient base-material working copy. Publication keeps source identity
	// and refuses to overwrite an externally changed source package.
	class FMaterialEditingSession
	{
	public:
		~FMaterialEditingSession();
		auto Initialize(DMaterial& Source, EMaterialEditCompileMode Mode,
			std::string& Error, DTransactor* InTransactor = nullptr) -> bool;
		auto GetWorkingMaterial() const -> DMaterial* { return Working.Get(); }
		auto GetSourceMaterial() const -> DMaterial* { return Source.Get(); }
		auto HasUnappliedChanges() const -> bool;
		auto IsApplyPending() const -> bool { return bApplyPending; }
		// Starts preview compilation when necessary; Tick publishes only its current result.
		auto RequestApply(std::string& Error) -> bool;
		auto Tick(std::string& Error) -> void;
		auto CancelApply() -> void { bApplyPending = false; }
		// Save's selected-owner barrier; never drains unrelated compilation.
		auto FinishAndApply(std::string& Error) -> bool;
		auto MarkSaved() -> void;

	private:
		struct FAuthoredState
		{
			FMaterialProgram Program;
			std::vector<FMaterialParameterDefinition> Definitions;
			FMaterialStaticProperties Properties;
			FMaterialGraphPresentation Presentation;
			auto operator==(const FAuthoredState&) const -> bool = default;
		};
		static auto Capture(const DMaterial& Material) -> FAuthoredState;
		auto ApplyCurrent(std::string& Error) -> bool;
		auto CheckSource(std::string& Error) const -> bool;
		TStrongObjectPtr<DMaterial> Source;
		TStrongObjectPtr<DPackage> WorkingPackage;
		TStrongObjectPtr<DMaterial> Working;
		TObjectPtr<DTransactor> Transactor;
		FAuthoredState Applied;
		uint64 SourceRevision = 0;
		uint64 RequestedWorkingRevision = 0;
		mutable uint64 ObservedWorkingRevision = 0;
		mutable bool bHasChanges = false;
		bool bApplyPending = false;
	};
}
