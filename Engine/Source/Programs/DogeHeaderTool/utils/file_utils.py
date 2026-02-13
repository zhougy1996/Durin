from dataclasses import dataclass
from pathlib import Path
import logging
import hashlib
from typing import Optional, Tuple

@dataclass
class LightFileFingerprint:
    timestamp: float
    file_size: int

@dataclass
class FileFingerprint:
    timestamp: float = 0.0
    file_size: int = 0
    md5: str = ""

@dataclass
class FileCacheEntry:
    content: str
    fingerprint: FileFingerprint

def calc_md5(file_path: Path, chunk_size: int = 8192) -> str:
    if not file_path.is_file():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    try:
        hash_obj = hashlib.md5()
        with open(file_path, "rb") as f:
            while chunk := f.read(chunk_size):
                hash_obj.update(chunk)
        return hash_obj.hexdigest()
    except (PermissionError, OSError) as e:
        raise IOError(f"Error reading file {file_path}: {e}")
    
def get_light_file_fingerprint(file_path: Path) -> LightFileFingerprint:
    if not file_path.is_file():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    try:
        timestamp = file_path.stat().st_mtime
        file_size = file_path.stat().st_size
        return LightFileFingerprint(timestamp=timestamp, file_size=file_size)
    except (PermissionError, OSError) as e:
        raise IOError(f"Error accessing file {file_path}: {e}")
    
def get_file_fingerprint(file_path: Path) -> FileFingerprint:
    if not file_path.is_file():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    
    try:
        timestamp = file_path.stat().st_mtime
        file_size = file_path.stat().st_size
        md5_hash = calc_md5(file_path)
        return FileFingerprint(timestamp=timestamp, file_size=file_size, md5=md5_hash)
    except (PermissionError, OSError) as e:
        raise IOError(f"Error accessing file {file_path}: {e}")

# Returns a tuple indicating whether the file has changed and the fingerprint of the file (either old or new)
def verify_file_fingerprint(file_path: Path, old_fingerprint: FileFingerprint) -> Tuple[bool, Optional[FileFingerprint]]:
    if not file_path.is_file():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    
    try:
        current_timestamp = file_path.stat().st_mtime
        current_file_size = file_path.stat().st_size
        
        if current_timestamp == old_fingerprint.timestamp and current_file_size == old_fingerprint.file_size:
            return False, old_fingerprint

        new_fingerprint = FileFingerprint(timestamp=current_timestamp, file_size=current_file_size, md5=calc_md5(file_path))
        return True, new_fingerprint
        
    except (PermissionError, OSError) as e:
        raise IOError(f"Error accessing file {file_path}: {e}")
    
def is_file_changed(file_path: Path, old_fingerprint: FileFingerprint) -> bool:
    changed, _ = verify_file_fingerprint(file_path, old_fingerprint)
    return changed

def calculate_file_hash(file_path: Path) -> str:
    if not file_path.exists():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    with open(file_path, "r", encoding="utf-8") as f:
        content = f.read()
    file_hash = hash(content)
    return str(file_hash)

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
