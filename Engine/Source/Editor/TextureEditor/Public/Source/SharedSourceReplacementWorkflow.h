#pragma once

#include "Misc/CoreStd.h"

namespace Durin::Editor::Texture
{
	// Advances shared-source replacement and recovery without waiting for an
	// asynchronous texture build on the editor thread.
	class FSharedSourceReplacementWorkflow final
	{
	public:
		enum class EPhase : uint8
		{
			Idle,
			BuildingReplacement,
			RestoringOriginal,
			Succeeded,
			Failed
		};

		struct FOperations
		{
			std::function<bool(std::string&)> Prepare;
			std::function<bool(std::string&)> StartReplacementBuild;
			std::function<bool()> IsBuildPending;
			std::function<bool(std::string&)> DidBuildSucceed;
			std::function<bool(std::string&)> Save;
			std::function<void()> Commit;
			std::function<void()> Rollback;
			std::function<bool(std::string&)> StartRestoreBuild;
		};

		auto Begin(FOperations InOperations) -> bool
		{
			if (IsBusy()) return false;
			Operations = std::move(InOperations);
			Error.clear();
			bRollbackPerformed = false;

			std::string OperationError;
			if (!Operations.Prepare(OperationError))
			{
				Fail(OperationError, "The shared source replacement could not be prepared.");
				return false;
			}
			if (!Operations.StartReplacementBuild(OperationError))
			{
				SetPrimaryError(OperationError, "The replacement texture build could not be started.");
				BeginRestore();
				return false;
			}
			Phase = EPhase::BuildingReplacement;
			return true;
		}

		auto Tick() -> void
		{
			if (Phase == EPhase::BuildingReplacement)
			{
				if (Operations.IsBuildPending()) return;
				std::string OperationError;
				if (!Operations.DidBuildSucceed(OperationError))
				{
					SetPrimaryError(OperationError, "The replacement texture build failed.");
					BeginRestore();
					return;
				}
				if (!Operations.Save(OperationError))
				{
					SetPrimaryError(OperationError, "The rebuilt texture could not be saved.");
					BeginRestore();
					return;
				}
				Operations.Commit();
				Phase = EPhase::Succeeded;
				return;
			}

			if (Phase != EPhase::RestoringOriginal || Operations.IsBuildPending()) return;
			std::string RestoreError;
			if (!Operations.DidBuildSucceed(RestoreError))
				AppendRestoreError(RestoreError, "The original texture state could not be rebuilt.");
			Phase = EPhase::Failed;
		}

		auto Abort() -> void
		{
			if (!IsBusy()) return;
			if (!bRollbackPerformed)
			{
				Operations.Rollback();
				bRollbackPerformed = true;
			}
			SetPrimaryError({}, "The shared source replacement was cancelled.");
			Phase = EPhase::Failed;
		}

		auto Reset() -> void
		{
			if (IsBusy()) return;
			Operations = {};
			Error.clear();
			Phase = EPhase::Idle;
			bRollbackPerformed = false;
		}

		auto GetPhase() const -> EPhase { return Phase; }
		auto GetError() const -> const std::string& { return Error; }
		auto IsBusy() const -> bool
		{
			return Phase == EPhase::BuildingReplacement
				|| Phase == EPhase::RestoringOriginal;
		}

	private:
		auto BeginRestore() -> void
		{
			Operations.Rollback();
			bRollbackPerformed = true;
			std::string RestoreError;
			if (!Operations.StartRestoreBuild(RestoreError))
			{
				AppendRestoreError(RestoreError, "The original texture build could not be started.");
				Phase = EPhase::Failed;
				return;
			}
			Phase = EPhase::RestoringOriginal;
		}

		auto SetPrimaryError(std::string_view Message, std::string_view Fallback) -> void
		{
			if (Error.empty()) Error = Message.empty() ? Fallback : Message;
		}

		auto AppendRestoreError(std::string_view Message, std::string_view Fallback) -> void
		{
			const std::string_view Detail = Message.empty() ? Fallback : Message;
			if (Error.empty()) Error = Detail;
			else Error += std::format(" Recovery also failed: {}", Detail);
		}

		auto Fail(std::string_view Message, std::string_view Fallback) -> void
		{
			SetPrimaryError(Message, Fallback);
			Phase = EPhase::Failed;
		}

		FOperations Operations;
		std::string Error;
		EPhase Phase = EPhase::Idle;
		bool bRollbackPerformed = false;
	};
}
