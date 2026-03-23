#!/usr/bin/env python3
"""
Sample List Management Tool
Interactive script to manage test samples (decode, encode, skip lists).

Usage:
    python3 manage_samples_list.py skip add       # Add skip entry
    python3 manage_samples_list.py decode add     # Add decode sample
    python3 manage_samples_list.py encode add     # Add encode sample
    python3 manage_samples_list.py <type> list    # List samples
    python3 manage_samples_list.py <type> remove  # Remove samples

Copyright 2025 Igalia S.L.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import argparse
import fnmatch
import json
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional

# Allow running both as package and as script (exception for this file only)
if __package__ is None or __package__ == "":
    sys.path.append(str(Path(__file__).resolve().parent.parent))

# pylint: disable=wrong-import-position
from tests.libs.video_test_utils import normalize_test_name  # noqa: E402


# Default file paths
DEFAULT_SKIP_LIST = Path(__file__).parent / "skipped_samples.json"
DEFAULT_DECODE_LIST = Path(__file__).parent / "decode_samples.json"
DEFAULT_ENCODE_LIST = Path(__file__).parent / "encode_samples.json"

# Valid field values for skip list
VALID_TEST_TYPES = ["decode", "encode"]
VALID_FORMATS = ["vvs", "fluster", "soothe"]
VALID_DRIVERS = ["all", "nvidia", "nvk", "amd", "radv", "intel", "anv"]
VALID_PLATFORMS = ["all", "linux", "windows"]
VALID_REPRODUCTION = ["always", "flaky"]

# Valid codecs for decode/encode samples
VALID_CODECS = ["h264", "h265", "av1", "vp9"]

# Valid H.264 profiles
VALID_H264_PROFILES = ["baseline", "main", "high", "high444"]


def load_skip_list(path: Path) -> Dict:
    """Load skip list JSON file"""
    if not path.exists():
        print(f"⚠️  Skip list not found: {path}")
        print("Creating new skip list...")
        return {
            "version": "1.0",
            "description": ("Test skip list for Vulkan Video Samples "
                            "test framework"),
            "decode": [],
            "encode": []
        }

    try:
        with open(path, 'r', encoding='utf-8') as f:
            data = json.load(f)
            # Migrate legacy format if needed
            if "skipped_tests" in data and "decode" not in data:
                print("⚠️  Migrating legacy skip list format...")
                data = migrate_skip_list_format(data)
            return data
    except json.JSONDecodeError as e:
        print(f"✗ Error parsing skip list: {e}")
        sys.exit(1)


def migrate_skip_list_format(data: Dict) -> Dict:
    """Migrate legacy skip list format to new format with separate sections"""
    new_data = {
        "version": data.get("version", "1.0"),
        "description": data.get("description", ""),
        "decode": [],
        "encode": []
    }

    for entry in data.get("skipped_tests", []):
        test_type = entry.pop("type", "decode")
        if test_type == "encode":
            new_data["encode"].append(entry)
        else:
            new_data["decode"].append(entry)

    return new_data


def save_skip_list(path: Path, data: Dict) -> None:
    """Save skip list JSON file with pretty formatting"""
    try:
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
            f.write('\n')  # Add trailing newline
        print(f"✓ Skip list saved to: {path}")
    except (OSError, IOError) as e:
        print(f"✗ Error saving skip list: {e}")
        sys.exit(1)


def load_sample_list(path: Path, list_type: str) -> Dict:
    """Load decode or encode sample list JSON file"""
    if not path.exists():
        print(f"⚠️  Sample list not found: {path}")
        print(f"Creating new {list_type} sample list...")
        return {"samples": []}

    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except json.JSONDecodeError as e:
        print(f"✗ Error parsing sample list: {e}")
        sys.exit(1)


def save_sample_list(path: Path, data: Dict, list_type: str) -> None:
    """Save decode or encode sample list JSON file"""
    try:
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
            f.write('\n')  # Add trailing newline
        print(f"✓ {list_type.capitalize()} sample list saved to: {path}")
    except (OSError, IOError) as e:
        print(f"✗ Error saving sample list: {e}")
        sys.exit(1)


def get_input(prompt: str, default: Optional[str] = None,
              choices: Optional[List[str]] = None,
              allow_empty: bool = False) -> str:
    """Get user input with optional default and validation"""
    if default is not None:
        prompt = f"{prompt} [{default}]"
    if choices:
        prompt = f"{prompt} ({'/'.join(choices)})"

    while True:
        value = input(f"{prompt}: ").strip()

        # Use default if provided and input is empty
        if not value and default is not None:
            return default

        # Validate choices if provided
        if choices and value and value not in choices:
            print(f"  Invalid choice. Must be one of: {', '.join(choices)}")
            continue

        # Allow empty if explicitly allowed or default is provided
        if value or default is not None or allow_empty:
            return value if value else (default if default is not None else "")

        print("  This field is required.")


def get_list_input(prompt: str, default: Optional[List[str]] = None,
                   choices: Optional[List[str]] = None) -> List[str]:
    """Get comma-separated list input from user"""
    default_str = ','.join(default) if default else None
    if default_str:
        prompt = f"{prompt} (comma-separated) [{default_str}]"
    else:
        prompt = f"{prompt} (comma-separated)"

    if choices:
        prompt = f"{prompt}\n  Available: {', '.join(choices)}"

    while True:
        value = input(f"{prompt}: ").strip()

        # Use default if provided and input is empty
        if not value and default:
            return default

        if not value:
            print("  This field is required.")
            continue

        # Parse comma-separated values
        items = [item.strip() for item in value.split(',')]

        # Validate choices if provided
        if choices:
            invalid = [item for item in items if item not in choices]
            if invalid:
                print(f"  Invalid values: {', '.join(invalid)}")
                print(f"  Must be one of: {', '.join(choices)}")
                continue

        return items


def add_skip_entry(  # pylint: disable=too-many-locals
        skip_list: Dict, preset_name: Optional[str] = None) -> None:
    """Interactively add a new skip entry

    Args:
        skip_list: The skip list dictionary to add to
        preset_name: Optional pre-filled test name (skips name prompt)
    """
    print("\n" + "=" * 70)
    print("ADD NEW SKIP ENTRY")
    print("=" * 70)

    # Required fields
    if preset_name:
        raw_name = preset_name
        print(f"Test name: {raw_name}")
    else:
        raw_name = get_input(
            "Test name (supports wildcards like 'av1_*_10bit')")

    # Auto-detect test type from name prefix
    detected_type = None
    if raw_name.startswith("decode_"):
        detected_type = "decode"
    elif raw_name.startswith("encode_"):
        detected_type = "encode"

    # Normalize name by removing decode_/encode_ prefix
    name = normalize_test_name(raw_name)
    if name != raw_name:
        print(f"  → Normalized name: '{raw_name}' → '{name}'")

    # Use detected type as default, or ask if not detected
    if detected_type:
        print(f"  → Auto-detected type: {detected_type}")
        test_type = get_input("Test type", default=detected_type,
                              choices=VALID_TEST_TYPES)
    else:
        test_type = get_input("Test type", choices=VALID_TEST_TYPES)
    test_format = get_input("Test format", default="vvs",
                            choices=VALID_FORMATS)

    # Optional fields with defaults
    drivers = get_list_input("Drivers to skip", default=["all"],
                             choices=VALID_DRIVERS)
    platforms = get_list_input("Platforms to skip", default=["all"],
                               choices=VALID_PLATFORMS)
    reproduction = get_input("Reproduction type", default="always",
                             choices=VALID_REPRODUCTION)

    # Metadata fields
    reason = get_input("Reason for skipping", allow_empty=True)
    bug_url = get_input("Bug URL (optional)", allow_empty=True)

    # Auto-generate date
    date_added = datetime.now().strftime("%Y-%m-%d")

    # Optional source filepath for context
    source_filepath = get_input(
        "Source filepath (optional, for reference)",
        allow_empty=True)

    # Create entry (without 'type' field - it's implied by the section)
    entry = {
        "name": name,
        "format": test_format,
        "drivers": drivers,
        "platforms": platforms,
        "reproduction": reproduction,
        "reason": reason,
        "bug_url": bug_url,
        "date_added": date_added,
    }

    # Add optional source_filepath if provided
    if source_filepath:
        entry["source_filepath"] = source_filepath

    # Show preview
    print("\n" + "-" * 70)
    print(f"ENTRY PREVIEW (will be added to '{test_type}' section):")
    print("-" * 70)
    print(json.dumps(entry, indent=2))
    print("-" * 70)

    # Confirm
    confirm = get_input("\nAdd this entry?", default="y", choices=["y", "n"])
    if confirm.lower() != 'y':
        print("✗ Cancelled")
        return

    # Add to appropriate section based on test type
    skip_list[test_type].append(entry)
    print(f"✓ Added skip entry for: {name} (in {test_type} section)")


def list_skip_entries(skip_list: Dict) -> None:
    """List all skip entries with formatting"""
    decode_entries = skip_list.get("decode", [])
    encode_entries = skip_list.get("encode", [])
    total = len(decode_entries) + len(encode_entries)

    if total == 0:
        print("No skip entries found.")
        return

    print("\n" + "=" * 70)
    print(f"SKIP LIST ENTRIES ({total} total)")
    print("=" * 70)

    idx = 1
    if decode_entries:
        print(f"\n--- DECODE ({len(decode_entries)} entries) ---")
        for entry in decode_entries:
            drivers_str = ', '.join(entry.get('drivers', ['all']))
            print(f"\n[{idx}] {entry['name']}")
            print(f"    Format: {entry.get('format', 'vvs')}, "
                  f"Drivers: {drivers_str}")
            if entry.get('reason'):
                print(f"    Reason: {entry['reason']}")
            if entry.get('bug_url'):
                print(f"    Bug: {entry['bug_url']}")
            if entry.get('date_added'):
                print(f"    Added: {entry['date_added']}")
            idx += 1

    if encode_entries:
        print(f"\n--- ENCODE ({len(encode_entries)} entries) ---")
        for entry in encode_entries:
            drivers_str = ', '.join(entry.get('drivers', ['all']))
            print(f"\n[{idx}] {entry['name']}")
            print(f"    Format: {entry.get('format', 'vvs')}, "
                  f"Drivers: {drivers_str}")
            if entry.get('reason'):
                print(f"    Reason: {entry['reason']}")
            if entry.get('bug_url'):
                print(f"    Bug: {entry['bug_url']}")
            if entry.get('date_added'):
                print(f"    Added: {entry['date_added']}")
            idx += 1


# pylint: disable=too-many-locals,too-many-statements,too-many-branches
def remove_skip_entries(skip_list: Dict) -> None:
    """Remove skip entries by name or driver"""
    print("\n" + "=" * 70)
    print("REMOVE SKIP ENTRIES")
    print("=" * 70)

    decode_entries = skip_list.get("decode", [])
    encode_entries = skip_list.get("encode", [])
    total = len(decode_entries) + len(encode_entries)

    if total == 0:
        print("No skip entries to remove.")
        return

    # Show current entries
    list_skip_entries(skip_list)

    print("\n" + "-" * 70)
    print("Remove by:")
    print("  1. Test name (exact or pattern)")
    print("  2. Driver name")
    print("  3. Entry number")
    print("-" * 70)

    choice = get_input("Select option", choices=["1", "2", "3"])

    # Track removals as (section, index) tuples
    to_remove: List[tuple] = []

    if choice == "1":
        # Remove by name
        name_pattern = get_input("Enter test name (exact match or wildcard)")
        # Normalize pattern to strip decode_/encode_ prefix
        normalized_pattern = normalize_test_name(name_pattern)

        for i, entry in enumerate(decode_entries):
            if fnmatch.fnmatch(entry['name'], normalized_pattern):
                to_remove.append(('decode', i))
                print(f"  Matched: {entry['name']} (decode)")

        for i, entry in enumerate(encode_entries):
            if fnmatch.fnmatch(entry['name'], normalized_pattern):
                to_remove.append(('encode', i))
                print(f"  Matched: {entry['name']} (encode)")

    elif choice == "2":
        # Remove by driver
        driver = get_input("Enter driver name", choices=VALID_DRIVERS)

        for i, entry in enumerate(decode_entries):
            if driver in entry.get('drivers', []):
                to_remove.append(('decode', i))
                print(f"  Matched: {entry['name']} (decode, drivers: "
                      f"{', '.join(entry['drivers'])})")

        for i, entry in enumerate(encode_entries):
            if driver in entry.get('drivers', []):
                to_remove.append(('encode', i))
                print(f"  Matched: {entry['name']} (encode, drivers: "
                      f"{', '.join(entry['drivers'])})")

    elif choice == "3":
        # Remove by entry number (combined list)
        num = get_input(f"Enter entry number (1-{total})")
        try:
            idx = int(num) - 1
            if idx < 0 or idx >= total:
                print(f"✗ Invalid entry number: {num}")
                return

            # Determine which section the entry is in
            if idx < len(decode_entries):
                to_remove.append(('decode', idx))
                print(f"  Selected: {decode_entries[idx]['name']} (decode)")
            else:
                encode_idx = idx - len(decode_entries)
                to_remove.append(('encode', encode_idx))
                print(f"  Selected: {encode_entries[encode_idx]['name']} "
                      "(encode)")
        except ValueError:
            print(f"✗ Invalid number: {num}")
            return

    if not to_remove:
        print("✗ No entries matched.")
        return

    # Confirm removal
    print(f"\n⚠️  Found {len(to_remove)} entries to remove.")
    confirm = get_input("Remove these entries?", default="n",
                        choices=["y", "n"])

    if confirm.lower() != 'y':
        print("✗ Cancelled")
        return

    # Group removals by section and remove in reverse order
    decode_to_remove = sorted(
        [idx for section, idx in to_remove if section == 'decode'],
        reverse=True
    )
    encode_to_remove = sorted(
        [idx for section, idx in to_remove if section == 'encode'],
        reverse=True
    )

    for idx in decode_to_remove:
        removed = decode_entries.pop(idx)
        print(f"  ✓ Removed: {removed['name']} (decode)")

    for idx in encode_to_remove:
        removed = encode_entries.pop(idx)
        print(f"  ✓ Removed: {removed['name']} (encode)")

    print(f"✓ Removed {len(to_remove)} entries")


def _validate_skip_entry(entry: Dict, section: str, index: int,
                         errors: List, warnings: List) -> None:
    """Validate a single skip entry"""
    entry_name = entry.get('name', f'{section} entry #{index}')

    # Required fields
    if not entry.get('name'):
        errors.append(f"{section} entry #{index}: Missing 'name' field")

    if 'format' not in entry:
        warnings.append(
            f"{entry_name} ({section}): Missing 'format' field "
            "(will default to 'vvs')")
    elif entry['format'] not in VALID_FORMATS:
        errors.append(
            f"{entry_name} ({section}): Invalid format '{entry['format']}'")

    # Validate drivers
    if 'drivers' in entry:
        invalid_drivers = [d for d in entry['drivers']
                           if d not in VALID_DRIVERS]
        if invalid_drivers:
            errors.append(
                f"{entry_name} ({section}): Invalid drivers: "
                f"{', '.join(invalid_drivers)}")

    # Validate platforms
    if 'platforms' in entry:
        invalid_platforms = [p for p in entry['platforms']
                             if p not in VALID_PLATFORMS]
        if invalid_platforms:
            errors.append(
                f"{entry_name} ({section}): Invalid platforms: "
                f"{', '.join(invalid_platforms)}")

    # Validate reproduction
    if ('reproduction' in entry and
            entry['reproduction'] not in VALID_REPRODUCTION):
        errors.append(
            f"{entry_name} ({section}): Invalid reproduction "
            f"'{entry['reproduction']}'")

    # Check for missing metadata
    if not entry.get('reason'):
        warnings.append(f"{entry_name} ({section}): Missing 'reason' field")
    if not entry.get('date_added'):
        warnings.append(
            f"{entry_name} ({section}): Missing 'date_added' field")


# pylint: disable=too-many-branches
def validate_skip_list(skip_list: Dict) -> bool:
    """Validate skip list format and entries"""
    print("\n" + "=" * 70)
    print("VALIDATING SKIP LIST")
    print("=" * 70)

    errors: List[str] = []
    warnings: List[str] = []

    # Check top-level structure
    if "decode" not in skip_list and "encode" not in skip_list:
        errors.append("Missing 'decode' and 'encode' sections")
        print("✗ ERRORS:")
        for error in errors:
            print(f"  - {error}")
        return False

    decode_entries = skip_list.get("decode", [])
    encode_entries = skip_list.get("encode", [])
    total = len(decode_entries) + len(encode_entries)
    print(f"Found {total} entries to validate "
          f"({len(decode_entries)} decode, {len(encode_entries)} encode)...\n")

    # Validate decode entries
    for i, entry in enumerate(decode_entries, 1):
        _validate_skip_entry(entry, "decode", i, errors, warnings)

    # Validate encode entries
    for i, entry in enumerate(encode_entries, 1):
        _validate_skip_entry(entry, "encode", i, errors, warnings)

    # Print results
    if errors:
        print("✗ ERRORS:")
        for error in errors:
            print(f"  - {error}")

    if warnings:
        print("\n⚠️  WARNINGS:")
        for warning in warnings:
            print(f"  - {warning}")

    if not errors and not warnings:
        print("✓ Skip list is valid!")
        return True

    if not errors:
        print("\n✓ Skip list is valid (with warnings)")
        return True

    print(f"\n✗ Skip list has {len(errors)} errors")
    return False


# ============================================================================
# Decode/Encode Sample Management Functions
# ============================================================================

def add_decode_sample(sample_list: Dict) -> None:
    """Interactively add a new decode sample"""
    print("\n" + "=" * 70)
    print("ADD NEW DECODE SAMPLE")
    print("=" * 70)

    # Required fields
    name = get_input("Sample name")
    codec = get_input("Codec", choices=VALID_CODECS)
    description = get_input("Description", allow_empty=True)

    # Source file information
    source_url = get_input("Source URL", allow_empty=True)
    source_checksum = get_input("Source checksum (SHA256)", allow_empty=True)
    source_filepath = get_input("Source filepath", allow_empty=True)

    # Optional fields
    expected_md5 = get_input("Expected output MD5 (optional)",
                             allow_empty=True)
    expected_crc = get_input("Expected output CRC (optional)",
                             allow_empty=True)

    # Extra args
    extra_args_str = get_input("Extra args (space-separated, optional)",
                               allow_empty=True)
    extra_args = extra_args_str.split() if extra_args_str else None

    # Create entry
    entry = {
        "name": name,
        "codec": codec,
        "description": description,
        "source_url": source_url,
        "source_checksum": source_checksum,
        "source_filepath": source_filepath,
    }

    if expected_md5:
        entry["expected_output_md5"] = expected_md5
    if expected_crc:
        entry["expected_output_crc"] = expected_crc
    if extra_args:
        entry["extra_args"] = extra_args

    # Show preview
    print("\n" + "-" * 70)
    print("ENTRY PREVIEW:")
    print("-" * 70)
    print(json.dumps(entry, indent=2))
    print("-" * 70)

    # Confirm
    confirm = get_input("\nAdd this sample?", default="y",
                        choices=["y", "n"])
    if confirm.lower() != 'y':
        print("✗ Cancelled")
        return

    # Add to sample list
    sample_list["samples"].append(entry)
    print(f"✓ Added decode sample: {name}")


def add_encode_sample(sample_list: Dict) -> None:
    """Interactively add a new encode sample"""
    print("\n" + "=" * 70)
    print("ADD NEW ENCODE SAMPLE")
    print("=" * 70)

    # Required fields
    name = get_input("Sample name")
    codec = get_input("Codec", choices=VALID_CODECS)
    description = get_input("Description", allow_empty=True)

    # Video dimensions
    width = get_input("Width (pixels)")
    height = get_input("Height (pixels)")

    # Profile (optional, codec-specific)
    profile = None
    if codec == "h264":
        profile = get_input("H.264 profile (optional)",
                            choices=VALID_H264_PROFILES + [""],
                            allow_empty=True)
        if not profile:
            profile = None

    # Source file information
    source_url = get_input("Source URL", allow_empty=True)
    source_checksum = get_input("Source checksum (SHA256)", allow_empty=True)
    source_filepath = get_input("Source filepath", allow_empty=True)

    # Extra args
    extra_args_str = get_input("Extra args (space-separated, optional)",
                               allow_empty=True)
    extra_args = extra_args_str.split() if extra_args_str else None

    # Create entry
    entry = {
        "name": name,
        "codec": codec,
        "profile": profile,
        "extra_args": extra_args,
        "description": description,
        "width": int(width),
        "height": int(height),
        "source_url": source_url,
        "source_checksum": source_checksum,
        "source_filepath": source_filepath,
    }

    # Show preview
    print("\n" + "-" * 70)
    print("ENTRY PREVIEW:")
    print("-" * 70)
    print(json.dumps(entry, indent=2))
    print("-" * 70)

    # Confirm
    confirm = get_input("\nAdd this sample?", default="y",
                        choices=["y", "n"])
    if confirm.lower() != 'y':
        print("✗ Cancelled")
        return

    # Add to sample list
    sample_list["samples"].append(entry)
    print(f"✓ Added encode sample: {name}")


def list_samples(sample_list: Dict, list_type: str) -> None:
    """List all decode or encode samples with formatting"""
    samples = sample_list.get("samples", [])

    if not samples:
        print(f"No {list_type} samples found.")
        return

    print("\n" + "=" * 70)
    print(f"{list_type.upper()} SAMPLES ({len(samples)} total)")
    print("=" * 70)

    for i, sample in enumerate(samples, 1):
        print(f"\n[{i}] {sample['name']}")
        print(f"    Codec: {sample.get('codec', 'N/A')}")
        if 'width' in sample and 'height' in sample:
            print(f"    Resolution: {sample['width']}x{sample['height']}")
        if sample.get('description'):
            print(f"    Description: {sample['description']}")
        if sample.get('source_filepath'):
            print(f"    Source: {sample['source_filepath']}")


def remove_samples(sample_list: Dict, list_type: str) -> None:
    """Remove decode or encode samples by name or number"""
    print("\n" + "=" * 70)
    print(f"REMOVE {list_type.upper()} SAMPLES")
    print("=" * 70)

    samples = sample_list.get("samples", [])
    if not samples:
        print(f"No {list_type} samples to remove.")
        return

    # Show current samples
    list_samples(sample_list, list_type)

    print("\n" + "-" * 70)
    print("Remove by:")
    print("  1. Sample name (exact or pattern)")
    print("  2. Sample number")
    print("-" * 70)

    choice = get_input("Select option", choices=["1", "2"])

    to_remove = []

    if choice == "1":
        # Remove by name
        name_pattern = get_input("Enter sample name (exact match or wildcard)")

        for i, sample in enumerate(samples):
            if fnmatch.fnmatch(sample['name'], name_pattern):
                to_remove.append(i)
                print(f"  Matched: {sample['name']}")

    elif choice == "2":
        # Remove by sample number
        num = get_input(f"Enter sample number (1-{len(samples)})")
        try:
            idx = int(num) - 1
            if 0 <= idx < len(samples):
                to_remove.append(idx)
                print(f"  Selected: {samples[idx]['name']}")
            else:
                print(f"✗ Invalid sample number: {num}")
                return
        except ValueError:
            print(f"✗ Invalid number: {num}")
            return

    if not to_remove:
        print("✗ No samples matched.")
        return

    # Confirm removal
    print(f"\n⚠️  Found {len(to_remove)} samples to remove.")
    confirm = get_input("Remove these samples?", default="n",
                        choices=["y", "n"])

    if confirm.lower() != 'y':
        print("✗ Cancelled")
        return

    # Remove in reverse order to maintain indices
    for idx in sorted(to_remove, reverse=True):
        removed = samples.pop(idx)
        print(f"  ✓ Removed: {removed['name']}")

    print(f"✓ Removed {len(to_remove)} samples")


def main():
    """Main entry point"""
    parser = argparse.ArgumentParser(
        description="Manage sample lists for Vulkan Video test framework",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s skip add               # Add a new skip entry (interactive)
  %(prog)s skip add test_name     # Add skip entry with name pre-filled
  %(prog)s skip remove            # Remove skip entries
  %(prog)s skip list              # List all skip entries
  %(prog)s skip validate          # Validate skip list
  %(prog)s decode add             # Add a new decode sample
  %(prog)s decode add sample_name # Add decode sample with name pre-filled
  %(prog)s decode list            # List decode samples
  %(prog)s encode add             # Add a new encode sample
  %(prog)s encode list            # List encode samples
        """
    )

    parser.add_argument(
        'list_type',
        choices=['skip', 'decode', 'encode'],
        help="Type of sample list to manage"
    )

    parser.add_argument(
        'command',
        choices=['add', 'remove', 'list', 'validate'],
        help="Command to execute"
    )

    parser.add_argument(
        'name',
        nargs='?',
        default=None,
        help="Test/sample name for 'add' command (optional, skips name prompt)"
    )

    parser.add_argument(
        '-f', '--file',
        type=Path,
        default=None,
        help=("Custom file path "
              "(auto-detected based on list_type if not specified)")
    )

    args = parser.parse_args()

    # Determine file path based on list type
    if args.file is None:
        if args.list_type == 'skip':
            args.file = DEFAULT_SKIP_LIST
        elif args.list_type == 'decode':
            args.file = DEFAULT_DECODE_LIST
        elif args.list_type == 'encode':
            args.file = DEFAULT_ENCODE_LIST

    # Execute command
    modified = False

    if args.list_type == 'skip':
        # Load skip list
        skip_list = load_skip_list(args.file)

        if args.command == 'add':
            add_skip_entry(skip_list, preset_name=args.name)
            modified = True

        elif args.command == 'remove':
            remove_skip_entries(skip_list)
            modified = True

        elif args.command == 'list':
            list_skip_entries(skip_list)

        elif args.command == 'validate':
            valid = validate_skip_list(skip_list)
            sys.exit(0 if valid else 1)

        # Save if modified
        if modified:
            save_skip_list(args.file, skip_list)

    elif args.list_type in ('decode', 'encode'):
        # Validate command is only for skip list
        if args.command == 'validate':
            print("✗ Validate command is only available for skip list")
            sys.exit(1)

        # Load decode or encode sample list
        sample_list = load_sample_list(args.file, args.list_type)

        if args.command == 'add':
            if args.list_type == 'decode':
                add_decode_sample(sample_list)
            else:
                add_encode_sample(sample_list)
            modified = True

        elif args.command == 'remove':
            remove_samples(sample_list, args.list_type)
            modified = True

        elif args.command == 'list':
            list_samples(sample_list, args.list_type)

        # Save if modified
        if modified:
            save_sample_list(args.file, sample_list, args.list_type)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n✗ Interrupted by user")
        sys.exit(1)
    except (OSError, IOError, ValueError) as e:
        print(f"\n✗ Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
