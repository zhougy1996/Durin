#include "Dialogs/MessageDialog.h"
#include "Misc/StringConvert.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Durin
{
	auto ShowMessageDialog(std::string_view Title, std::string_view Message,
		bool Decline, bool Interactive) -> EMessageDialogResult
	{
		if (!Interactive) return EMessageDialogResult::Error;
#if defined(_WIN32)
		const int Result = ::MessageBoxW(nullptr, StringUtils::Utf8ToWide(Message).c_str(),
			StringUtils::Utf8ToWide(Title).c_str(), (Decline ? MB_YESNOCANCEL : MB_OKCANCEL) | MB_ICONQUESTION | MB_DEFBUTTON2);
		if (Result == IDYES || Result == IDOK) return EMessageDialogResult::Yes;
		if (Result == IDNO) return EMessageDialogResult::No;
		return Result == IDCANCEL ? EMessageDialogResult::Cancel : EMessageDialogResult::Error;
#elif defined(__APPLE__)
		if (!pthread_main_np()) return EMessageDialogResult::Error;
		auto Escape = [](std::string_view Value) {
			std::string Result = "\"";
			for (char C : Value) { if (C == '\\' || C == '"') Result += '\\'; Result += C; }
			return Result + "\"";
		};
		const std::string Script = "set answer to button returned of (display dialog " + Escape(Message)
			+ " with title " + Escape(Title) + " buttons " + (Decline ? "{\"Cancel\", \"No\", \"Yes\"}" : "{\"Cancel\", \"Yes\"}")
			+ " default button \"Cancel\")\nif answer is \"Yes\" then\nreturn 0\nelse if answer is \"No\" then\nreturn 1\nelse\nreturn 2\nend if";
		int Pipe[2];
		if (pipe(Pipe)) return EMessageDialogResult::Error;
		posix_spawn_file_actions_t Actions;
		posix_spawn_file_actions_init(&Actions);
		posix_spawn_file_actions_adddup2(&Actions, Pipe[1], STDOUT_FILENO);
		posix_spawn_file_actions_addclose(&Actions, Pipe[0]);
		posix_spawn_file_actions_addclose(&Actions, Pipe[1]);
		const char* Args[]{"/usr/bin/osascript", "-e", Script.c_str(), nullptr};
		pid_t Child;
		const int Spawn = posix_spawn(&Child, Args[0], &Actions, nullptr, const_cast<char* const*>(Args), environ);
		posix_spawn_file_actions_destroy(&Actions); close(Pipe[1]);
		char Answer[16]{};
		const auto Count = Spawn ? -1 : read(Pipe[0], Answer, sizeof(Answer));
		close(Pipe[0]);
		if (Spawn) return EMessageDialogResult::Error;
		int Status = 0;
		while (waitpid(Child, &Status, 0) < 0) { if (errno != EINTR) return EMessageDialogResult::Error; }
		if (Count <= 0 || !WIFEXITED(Status) || WEXITSTATUS(Status)) return EMessageDialogResult::Cancel;
		return Answer[0] == '0' ? EMessageDialogResult::Yes : Answer[0] == '1' ? EMessageDialogResult::No : EMessageDialogResult::Cancel;
#else
		return EMessageDialogResult::Error;
#endif
	}
}
