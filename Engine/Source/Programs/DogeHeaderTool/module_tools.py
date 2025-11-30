import json, sys
import argparse
import os

def get_dht_headers(module_dir) -> list:
    module_file = os.path.join(module_dir, f"{os.path.basename(module_dir)}.dmodule")
    if not os.path.isfile(module_file):
        return []

    
    with open(module_file, "r") as f:
        # return empty if module file is empty
        content = f.read()
        if not content:
            return []
        
        module_data = json.loads(content)
        dht_headers = module_data.get("DHTHeaders", [])
        return dht_headers

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="DHT tools")
    parser.add_argument("--module_dir", help="Module directory", required=True)
    parser.add_argument("--function", help="Function to perform", choices=["get_dht_headers"], required=True)
    args = parser.parse_args()

    if args.function == "get_dht_headers":
        headers = get_dht_headers(args.module_dir)
        if headers:
            print(";".join(headers))
        else:
            print("\n")