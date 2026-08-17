#include <fstream>
#include <iostream>
#include <string_view>

int main(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount < 5 || std::string_view(Arguments[1]) != "--log"
		|| std::string_view(Arguments[3]) != "--")
	{
		return 2;
	}
	bool Discovery = false;
	bool Case = false;
	bool WholeTarget = false;
	for (int Index = 4; Index < ArgumentCount; ++Index)
	{
		const std::string_view Argument = Arguments[Index];
		Discovery = Discovery || Argument == "--gtest_list_tests";
		Case = Case || Argument.starts_with("--gtest_filter=ProbeSuite.Pass");
		WholeTarget = WholeTarget || Argument == "--durin-whole-target";
	}
	std::ofstream Log(Arguments[2], std::ios::app);
	Log << (Discovery ? "discovery" : Case ? "case" : WholeTarget ? "whole-target" : "unknown")
		<< '\n';
	if (Discovery) std::cout << "ProbeSuite.\n  Pass\n";
	return Discovery || Case || WholeTarget ? 0 : 3;
}
