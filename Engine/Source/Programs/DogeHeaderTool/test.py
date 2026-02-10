import sys

import main

sys.argv = ["main.py", "generate_module_cmake_data", "-m", "Core"]

if __name__ == "__main__":
    main.main()