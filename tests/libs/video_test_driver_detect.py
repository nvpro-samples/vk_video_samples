"""
Video Driver Detection Utilities
Maps Vulkan vendor/device IDs to driver names for skip list filtering.

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

import platform
import re
from dataclasses import dataclass
from typing import Optional


@dataclass
class SystemInfo:
    """System information detected from test output."""
    gpu_name: str = ""
    driver_name: str = ""
    driver_version: str = ""
    os_name: str = ""

    def get_header(self) -> str:
        """Generate header string: GPU Model / Driver Version / OS"""
        parts = []
        if self.gpu_name:
            parts.append(self.gpu_name)
        if self.driver_name or self.driver_version:
            driver_str = self.driver_name
            if self.driver_version:
                driver_str = f"{driver_str} {self.driver_version}".strip()
            if driver_str:
                parts.append(driver_str)
        if self.os_name:
            parts.append(self.os_name)
        return " / ".join(parts) if parts else "Unknown System"

    def is_empty(self) -> bool:
        """Check if system info has any data."""
        return not any([self.gpu_name, self.driver_name,
                       self.driver_version, self.os_name])


def get_os_info() -> str:
    """Get OS name and version string."""
    system = platform.system()
    if system == "Linux":
        try:
            # Try to get distro info from /etc/os-release
            with open("/etc/os-release", encoding="utf-8") as f:
                os_release = {}
                for line in f:
                    if "=" in line:
                        key, value = line.strip().split("=", 1)
                        os_release[key] = value.strip('"')
                distro = os_release.get("PRETTY_NAME",
                                        os_release.get("NAME", "Linux"))
                return distro
        except (OSError, IOError):
            return f"Linux {platform.release()}"
    elif system == "Windows":
        return f"Windows {platform.release()}"
    elif system == "Darwin":
        return f"macOS {platform.mac_ver()[0]}"
    return system


# Vulkan Vendor IDs (PCI vendor IDs)
VENDOR_NVIDIA = 0x10DE
VENDOR_AMD = 0x1002
VENDOR_INTEL = 0x8086
VENDOR_ARM = 0x13B5
VENDOR_QUALCOMM = 0x5143
VENDOR_BROADCOM = 0x14E4
VENDOR_MESA = 0x10005  # Software renderer


class DriverMapping:
    """Maps Vulkan vendor/device IDs to driver names for Linux and Windows"""

    # Vendor ID to driver mapping
    # Format: {vendor_id: {"linux": "driver_name", "windows": "driver_name"}}
    VENDOR_TO_DRIVER = {
        VENDOR_NVIDIA: {
            "linux": "nvidia",      # Default to proprietary on Linux
            "windows": "nvidia"     # Always proprietary on Windows
        },
        VENDOR_AMD: {
            "linux": "radv",        # Default to open-source on Linux
            "windows": "amd"        # Always proprietary on Windows
        },
        VENDOR_INTEL: {
            "linux": "anv",         # Default to open-source on Linux
            "windows": "intel"      # Always proprietary on Windows
        },
        VENDOR_ARM: {
            "linux": "arm",
            "windows": "arm"
        },
        VENDOR_QUALCOMM: {
            "linux": "qualcomm",
            "windows": "qualcomm"
        },
        VENDOR_BROADCOM: {
            "linux": "broadcom",
            "windows": "broadcom"
        },
        VENDOR_MESA: {
            "linux": "mesa",
            "windows": "mesa"
        }
    }

    # Driver ID to driver name mapping (from VK_KHR_driver_properties)
    # These are more precise than vendor IDs
    # See: https://registry.khronos.org/vulkan/specs/1.3-extensions/
    #      man/html/VkDriverId.html
    DRIVER_ID_TO_NAME = {
        1: "amd",                    # VK_DRIVER_ID_AMD_PROPRIETARY
        2: "amd",                    # VK_DRIVER_ID_AMD_OPEN_SOURCE
        3: "radv",                   # VK_DRIVER_ID_MESA_RADV
        4: "nvidia",                 # VK_DRIVER_ID_NVIDIA_PROPRIETARY
        5: "intel",                  # VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS
        6: "anv",                    # VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA
        7: "imagination",            # VK_DRIVER_ID_IMAGINATION_PROPRIETARY
        8: "qualcomm",               # VK_DRIVER_ID_QUALCOMM_PROPRIETARY
        9: "arm",                    # VK_DRIVER_ID_ARM_PROPRIETARY
        10: "google_swiftshader",    # VK_DRIVER_ID_GOOGLE_SWIFTSHADER
        11: "ggp",                   # VK_DRIVER_ID_GGP_PROPRIETARY
        12: "broadcom",              # VK_DRIVER_ID_BROADCOM_PROPRIETARY
        13: "mesa_llvmpipe",         # VK_DRIVER_ID_MESA_LLVMPIPE
        14: "moltenvk",              # VK_DRIVER_ID_MOLTENVK
        15: "coreavi",               # VK_DRIVER_ID_COREAVI_PROPRIETARY
        16: "juice",                 # VK_DRIVER_ID_JUICE_PROPRIETARY
        17: "verisilicon",           # VK_DRIVER_ID_VERISILICON_PROPRIETARY
        18: "mesa_turnip",           # VK_DRIVER_ID_MESA_TURNIP
        19: "mesa_v3dv",             # VK_DRIVER_ID_MESA_V3DV
        20: "mesa_panvk",            # VK_DRIVER_ID_MESA_PANVK
        21: "samsung",               # VK_DRIVER_ID_SAMSUNG_PROPRIETARY
        22: "mesa_venus",            # VK_DRIVER_ID_MESA_VENUS
        23: "mesa_dozen",            # VK_DRIVER_ID_MESA_DOZEN
        24: "mesa_nvk",              # VK_DRIVER_ID_MESA_NVK
        25: "imagination_open",      # VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE
        26: "mesa_honeykrisp",       # VK_DRIVER_ID_MESA_HONEYKRISP
        27: "vulkan_sc_emu_google",  # VK_DRIVER_ID_RESERVED_27
    }

    # Normalized driver names for skip list matching
    # Maps various driver identifiers to standard names
    DRIVER_NAME_MAPPING = {
        # NVIDIA
        "nvidia": "nvidia",
        "nvidia proprietary": "nvidia",
        "nvk": "nvk",
        "mesa_nvk": "nvk",
        "mesa nvk": "nvk",
        "nouveau": "nvk",

        # AMD
        "amd": "amd",
        "amd proprietary": "amd",
        "amd open source": "radv",
        "radv": "radv",
        "mesa_radv": "radv",
        "mesa radv": "radv",
        "amdvlk": "amd",

        # Intel
        "intel": "intel",
        "intel proprietary": "intel",
        "intel open source": "anv",
        "intel open source mesa": "anv",
        "intel anv": "anv",
        "anv": "anv",
        "mesa anv": "anv",

        # Other
        "arm": "arm",
        "qualcomm": "qualcomm",
        "broadcom": "broadcom",
        "mesa": "mesa",
        "swiftshader": "swiftshader",
        "llvmpipe": "llvmpipe",
    }

    @staticmethod
    def get_platform() -> str:
        """Get current platform (linux or windows)"""
        system = platform.system().lower()
        if system == "windows":
            return "windows"
        return "linux"  # Default to Linux for Unix-like systems

    @staticmethod
    def vendor_id_to_driver(vendor_id: int,
                            platform_name: Optional[str] = None) -> str:
        """
        Convert Vulkan vendor ID to driver name.

        Args:
            vendor_id: Vulkan vendor ID (PCI vendor ID)
            platform_name: Platform name ("linux" or "windows").
                          If None, auto-detect current platform.

        Returns:
            Driver name string (e.g., "nvidia", "radv", "anv")
            Returns "unknown" if vendor is not recognized.
        """
        if platform_name is None:
            platform_name = DriverMapping.get_platform()

        vendor_mapping = DriverMapping.VENDOR_TO_DRIVER.get(vendor_id)
        if vendor_mapping:
            return vendor_mapping.get(platform_name, "unknown")

        return "unknown"

    @staticmethod
    def driver_id_to_driver(driver_id: int) -> str:
        """
        Convert Vulkan driver ID to driver name.

        This is more precise than vendor ID mapping and should be
        preferred when available (Vulkan 1.2+ with VK_KHR_driver_properties).

        Args:
            driver_id: Vulkan driver ID from VkPhysicalDeviceDriverProperties

        Returns:
            Driver name string (e.g., "nvidia", "radv", "nvk", "anv")
            Returns "unknown" if driver ID is not recognized.
        """
        return DriverMapping.DRIVER_ID_TO_NAME.get(driver_id, "unknown")

    @staticmethod
    def normalize_driver_name(driver_name: str) -> str:
        """
        Normalize driver name to standard form for skip list matching.

        Args:
            driver_name: Raw driver name string (case-insensitive)

        Returns:
            Normalized driver name (e.g., "nvidia", "radv", "anv", "nvk")
        """
        normalized = driver_name.lower().strip()
        return DriverMapping.DRIVER_NAME_MAPPING.get(normalized, normalized)

    @staticmethod
    def detect_driver_from_vendor(vendor_id: int,
                                  device_id: int,
                                  platform_name: Optional[str] = None) -> str:
        """
        Detect driver name from vendor and device IDs.

        This is a fallback when driver ID is not available.

        Args:
            vendor_id: Vulkan vendor ID (PCI vendor ID)
            device_id: Vulkan device ID (PCI device ID) - currently unused
                      but reserved for future device-specific detection
            platform_name: Platform name ("linux" or "windows").
                          If None, auto-detect current platform.

        Returns:
            Driver name string (e.g., "nvidia", "radv", "anv")
        """
        # Currently we only use vendor_id, but device_id is here for
        # future extensions (e.g., detecting specific driver variants)
        _ = device_id  # Unused for now
        return DriverMapping.vendor_id_to_driver(vendor_id, platform_name)

    @staticmethod
    def get_vendor_name(vendor_id: int) -> str:
        """
        Get human-readable vendor name from vendor ID.

        Args:
            vendor_id: Vulkan vendor ID (PCI vendor ID)

        Returns:
            Vendor name string (e.g., "NVIDIA", "AMD", "Intel")
        """
        vendor_names = {
            VENDOR_NVIDIA: "NVIDIA",
            VENDOR_AMD: "AMD",
            VENDOR_INTEL: "Intel",
            VENDOR_ARM: "ARM",
            VENDOR_QUALCOMM: "Qualcomm",
            VENDOR_BROADCOM: "Broadcom",
            VENDOR_MESA: "Mesa Software Renderer"
        }
        return vendor_names.get(vendor_id, f"Unknown (0x{vendor_id:04X})")


def parse_system_info_from_output(stdout: str, stderr: str = "") -> SystemInfo:
    """
    Parse full system information from test executable output.

    Extracts GPU name, driver name, driver version, and OS info.

    Args:
        stdout: Standard output from test executable
        stderr: Standard error from test executable (optional)

    Returns:
        SystemInfo object with detected information

    Example output line:
        *** Selected Vulkan physical device with name: NVIDIA GeForce RTX 3080,
        vendor ID: 0x10de, device UUID: ..., and device ID: 0x2206,
        driver ID: 5, driver name: NVIDIA,
        Num Decode Queues: 16, Num Encode Queues: 3 ***
    """
    combined_output = stdout + "\n" + stderr
    info = SystemInfo(os_name=get_os_info())

    # Extract GPU name from "device with name: XXX,"
    gpu_pattern = r'device with name:\s*([^,]+)'
    gpu_match = re.search(gpu_pattern, combined_output, re.IGNORECASE)
    if gpu_match:
        info.gpu_name = gpu_match.group(1).strip()

    # Extract driver name
    driver_name_pattern = r'driver name:\s*([^,\n*]+)'
    driver_match = re.search(driver_name_pattern, combined_output,
                             re.IGNORECASE)
    if driver_match:
        info.driver_name = driver_match.group(1).strip()

    # Extract driver version if present (format varies by vendor)
    # NVIDIA format: "driver version: 550.120"
    # Mesa format: "driver info: Mesa 24.0.0"
    version_pattern = r'driver version:\s*([^\n,*]+)'
    version_match = re.search(version_pattern, combined_output, re.IGNORECASE)
    if version_match:
        info.driver_version = version_match.group(1).strip()
    else:
        # Try driver info pattern (Mesa)
        driver_info_pattern = r'driver info:\s*([^\n,*]+)'
        info_match = re.search(driver_info_pattern, combined_output,
                               re.IGNORECASE)
        if info_match:
            info.driver_version = info_match.group(1).strip()

    return info


def parse_driver_from_output(stdout: str, stderr: str = "") -> Optional[str]:
    """
    Parse driver information from test executable output.

    Looks for the line:
    "*** Selected Vulkan physical device with name: ...
    vendor ID: 0xXXXX ... device ID: 0xXXXX ***"

    Preferably with driver ID and driver name for more accurate
    detection (Vulkan 1.2+):
    "*** Selected Vulkan physical device with name: ...,
    vendor ID: 0x10de, device UUID: ..., device ID: 0x2206,
    driver ID: 5, driver name: NVIDIA ***"

    Args:
        stdout: Standard output from test executable
        stderr: Standard error from test executable (optional)

    Returns:
        Detected driver name (e.g., "nvidia", "radv", "anv") or None

    Example output line:
        *** Selected Vulkan physical device with name: NVIDIA GeForce RTX 3080,
        vendor ID: 0x10de, device UUID: ..., and device ID: 0x2206,
        driver ID: 5, driver name: NVIDIA,
        Num Decode Queues: 16, Num Encode Queues: 3 ***
    """
    # Combine stdout and stderr for searching
    combined_output = stdout + "\n" + stderr

    # Pattern to match the device selection line with optional driver info
    # Looks for: vendor ID: 0xXXXX, device ID: 0xXXXX,
    # optionally driver ID and driver name
    pattern = (r'vendor ID:\s*(?:0x)?([0-9a-fA-F]+).*?'
               r'device ID:\s*(?:0x)?([0-9a-fA-F]+)'
               r'(?:.*?driver ID:\s*(\d+))?'
               r'(?:.*?driver name:\s*([^,\n]+))?')

    match = re.search(pattern, combined_output, re.IGNORECASE | re.DOTALL)
    if not match:
        return None

    try:
        vendor_id = int(match.group(1), 16)
        device_id = int(match.group(2), 16)

        # Extract driver ID and driver name if available (groups 3 and 4)
        driver_id = None
        driver_name = None

        if match.group(3):  # driver ID
            driver_id = int(match.group(3))

        if match.group(4):  # driver name
            driver_name = match.group(4).strip()

        # Use parse_driver_info with all available information
        return parse_driver_info(vendor_id, device_id,
                                 driver_id=driver_id,
                                 driver_name=driver_name)
    except (ValueError, IndexError):
        return None


def parse_driver_info(vendor_id: int,
                      device_id: int,
                      driver_id: Optional[int] = None,
                      driver_name: Optional[str] = None,
                      platform_name: Optional[str] = None) -> str:
    """
    Parse driver information and return normalized driver name.

    This is the main entry point for driver detection. It uses the most
    specific information available, preferring:
    1. Driver ID (most specific, Vulkan 1.2+)
    2. Driver name string
    3. Vendor ID (fallback)

    Args:
        vendor_id: Vulkan vendor ID (PCI vendor ID)
        device_id: Vulkan device ID (PCI device ID)
        driver_id: Optional Vulkan driver ID from
                   VkPhysicalDeviceDriverProperties
        driver_name: Optional driver name string from
                    VkPhysicalDeviceDriverProperties
        platform_name: Platform name ("linux" or "windows").
                      If None, auto-detect current platform.

    Returns:
        Normalized driver name for skip list matching
        (e.g., "nvidia", "radv", "anv", "nvk")

    Examples:
        >>> parse_driver_info(0x10DE, 0x1234, driver_id=24)  # NVIDIA NVK
        'nvk'
        >>> parse_driver_info(0x1002, 0x5678, driver_id=3)   # AMD RADV
        'radv'
        >>> parse_driver_info(0x8086, 0x9ABC)                # Intel (fallback)
        'anv'
    """
    # Prefer driver ID if available (most specific)
    if driver_id is not None:
        detected = DriverMapping.driver_id_to_driver(driver_id)
        if detected != "unknown":
            return detected

    # Try driver name string if available
    if driver_name:
        return DriverMapping.normalize_driver_name(driver_name)

    # Fallback to vendor ID
    return DriverMapping.detect_driver_from_vendor(
        vendor_id, device_id, platform_name
    )
