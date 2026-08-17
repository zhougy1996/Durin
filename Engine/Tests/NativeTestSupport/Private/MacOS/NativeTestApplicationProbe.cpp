#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>

int main(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount != 2) return 64;
	const std::string_view Mode = Arguments[1];
	std::cout << "application-probe-stdout:" << Mode << '\n';
	std::cerr << "application-probe-stderr:" << Mode << '\n';
	if (Mode == "pass") return 0;
	if (Mode == "fail") return 7;
	if (Mode == "crash")
	{
		raise(SIGABRT);
		return 70;
	}
	if (Mode == "hang")
	{
		for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	return 64;
}
