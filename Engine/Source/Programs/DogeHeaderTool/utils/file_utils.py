from pathlib import Path
import logging

def generate_file(file_path: Path, content: str, compare: bool = True) -> None:
    file_path.parent.mkdir(parents=True, exist_ok=True)
    # Only write file if content has changed
    if compare and file_path.exists():
        with open(file_path, "r", encoding="utf-8") as f:
            existing_content = f.read()
            if existing_content == content:
                logging.debug(f"No changes detected for {file_path}. Skipping write.")
                return
    # Write new content to file no matter what
    with open(file_path, "w", encoding="utf-8") as f:
        f.write(content)
