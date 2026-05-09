#pragma once

struct ImGuiContext;

namespace Doge::Mona
{
	class FMonaApplication;

	extern ImGuiContext* GMonaImGuiContext;

	auto BackendInit() -> void;

	auto BackendClose() -> void;

	// Initialize the Mona event handler and set it to the application. This should be called after the application is initialized.
	auto InitMonaBackendEventHandler() -> void;
}