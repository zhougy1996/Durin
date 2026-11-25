#pragma once

#include "CoreMinimal.h"
#include "DObject/StrongObjectPtr.h"
#include "Materials/Material.h"

namespace Durin { class DTransactor; }

namespace Durin::MaterialEditor
{
	class DMaterialGraph;
	class FMaterialGraphCanvas;
	class FMaterialPreview;

	// Owns a transient base-material working copy. Publication keeps source identity
	// and refuses to overwrite an externally edited source material.
	class FMaterialEditingSession
	{
		friend class FMaterialEditorReloadParticipant;
	public:
		FMaterialEditingSession();
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

		TStrongObjectPtr<DMaterialGraph> Graph;
		std::unique_ptr<FMaterialGraphCanvas> Canvas;
		std::shared_ptr<FMaterialPreview> Preview;

	private:
		auto HasSemanticChanges() const -> bool;
		auto ApplyCurrent(std::string& Error) -> bool;
		auto CheckSource(std::string& Error) const -> bool;
		TStrongObjectPtr<DMaterial> Source;
		TStrongObjectPtr<DPackage> WorkingPackage;
		TStrongObjectPtr<DMaterial> Working;
		TObjectPtr<DTransactor> Transactor;
		uint64 SourceRevision = 0;
		uint64 RequestedWorkingRevision = 0;
		uint64 PublishedWorkingRevision = 0;
		bool bPublishing = false;
		bool bApplyPending = false;
	};
}
