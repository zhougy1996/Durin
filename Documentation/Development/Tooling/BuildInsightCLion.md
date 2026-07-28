# BuildInsight In CLion

BuildInsight is a tool that helps you analyze and optimize your build times. It provides insights into which files are taking the longest to compile and which dependencies are causing the most overhead. By using BuildInsight, you can identify bottlenecks in your build process and make informed decisions about how to improve it.

To use BuildInsight in CLion, you need to set up a external tool that will run the BuildInsight command line interface (CLI) with the appropriate arguments. Here are the steps to do this:
1. Open CLion and go to `File > Settings > Tools > External Tools`.
2. Click the `+` button to add new external tools for Debug and Release configurations.
3. Fill in the fields as follows(take Debug configuration as an example):
   - **Name**: BuildInsight_Debug
   - **Program**: cmd.exe
   - **Arguments**: "path\to\Durin\Engine\Scripts\Build\BuildInsight.bat" Debug"
