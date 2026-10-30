"""
Vulkan Video Validation Layer Check
Runs an encode under the Khronos validation layer and sorts what the layer
reports.

The layer is the one the Vulkan loader loads: the first manifest of
VK_LAYER_KHRONOS_validation in VK_ADD_LAYER_PATH, then in VK_LAYER_PATH or,
when that is not set, in the loader's default directories. A layer that is
in none of them is found in VULKAN_SDK, and the encode runs with its
directory added to VK_ADD_LAYER_PATH. The loader's registry entries on
Windows are not read: there the layer is found through those variables.

The layer writes its messages to a log file of its own, which it creates
when the encoder creates its Vulkan instance, empty when it has nothing to
report. An encode that leaves no log never loaded the layer, and fails the
check instead of passing unseen.

An encode passes the check when the layer reports no error except those on
the allowlist, validation_allowlist.json next to the samples. An entry names
a message by its VUID, which may hold shell wildcards, and may narrow it to
codecs, to the layer versions that report it (the api_version of the layer's
manifest) and to a substring of the message.

Copyright 2026 NVIDIA Corporation

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

import fnmatch
import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Mapping, Optional, Tuple

LAYER_NAME = "VK_LAYER_KHRONOS_validation"
DEFAULT_ALLOWLIST = Path(__file__).parent.parent / "validation_allowlist.json"

Version = Tuple[int, int, int]

# The first line of each message the layer logs. The text follows on the same
# line or on the next ones, and repeats the VUID in its link to the
# specification, which is not a message of its own.
_HEADER = re.compile(
    r"^Validation (Error|Warning|Performance Warning|Information): \[ (\S+) \]",
    re.MULTILINE)

_ALLOWLIST_FILE_KEYS = frozenset({"version", "description", "encode"})
_ALLOWLIST_ENTRY_KEYS = frozenset({
    "vuid", "reason", "codecs", "message", "min_layer_version",
    "max_layer_version", "bug_url"})


def parse_version(text) -> Version:
    """The major, minor and patch numbers of a layer version, "1.4.357"."""
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", str(text).strip())
    if not match:
        raise ValueError(f"not a layer version: {text!r}")
    major, minor, patch = (int(number) for number in match.groups())
    return major, minor, patch


def version_text(version: Version) -> str:
    """A version as a manifest writes it."""
    return ".".join(str(number) for number in version)


@dataclass(frozen=True)
class LayerManifest:
    """The manifest of the validation layer an encode loads, and its version.

    add_to_search_path marks a manifest the loader does not find by itself,
    found in VULKAN_SDK: the encode runs with its directory added to
    VK_ADD_LAYER_PATH.
    """
    path: Path
    api_version: Version
    add_to_search_path: bool = False


def _read_manifest(path: Path) -> Optional[Version]:
    """The api_version of the validation layer in a manifest file, or None
    when the file is not a manifest of the layer."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    layers = data.get("layers")
    if not isinstance(layers, list):
        layers = [data.get("layer")]
    for layer in layers:
        if isinstance(layer, dict) and layer.get("name") == LAYER_NAME:
            try:
                return parse_version(layer.get("api_version", ""))
            except ValueError:
                return None
    return None


def _manifests(place: Path) -> List[Path]:
    """The manifests a search path names: a file, or the .json files of a
    directory in name order."""
    if place.is_file():
        return [place]
    if place.is_dir():
        return sorted(path for path in place.glob("*.json") if path.is_file())
    return []


def _split_paths(value: Optional[str]) -> List[Path]:
    """The paths of a search path variable."""
    return [Path(part) for part in (value or "").split(os.pathsep) if part]


def _default_dirs(environ: Mapping[str, str]) -> List[Path]:
    """The explicit layer directories of the loader on Linux, in its order."""
    if os.name == "nt":
        return []
    home = environ.get("HOME", "")
    bases = [environ.get("XDG_CONFIG_HOME") or (home and f"{home}/.config")]
    bases += (environ.get("XDG_CONFIG_DIRS") or "/etc/xdg").split(":")
    bases += ["/usr/local/etc", "/etc"]
    bases.append(environ.get("XDG_DATA_HOME") or (home and f"{home}/.local/share"))
    bases += (environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":")
    return [Path(base, "vulkan", "explicit_layer.d") for base in bases if base]


def _sdk_dir(environ: Mapping[str, str]) -> Optional[Path]:
    """Where the Vulkan SDK in VULKAN_SDK keeps the layer's manifest."""
    sdk = environ.get("VULKAN_SDK")
    if not sdk:
        return None
    if os.name == "nt":
        return Path(sdk, "Bin")
    return Path(sdk, "share", "vulkan", "explicit_layer.d")


def find_validation_layer(
        environ: Optional[Mapping[str, str]] = None) -> Optional[LayerManifest]:
    """The manifest of the validation layer an encode loads, or None when the
    layer is neither where the loader looks nor in VULKAN_SDK."""
    environ = os.environ if environ is None else environ
    places = _split_paths(environ.get("VK_ADD_LAYER_PATH"))
    layer_path = environ.get("VK_LAYER_PATH")
    places += _split_paths(layer_path) if layer_path else _default_dirs(environ)
    for place in places:
        for manifest in _manifests(place):
            version = _read_manifest(manifest)
            if version is not None:
                return LayerManifest(manifest, version)
    sdk_dir = _sdk_dir(environ)
    for manifest in _manifests(sdk_dir) if sdk_dir else []:
        version = _read_manifest(manifest)
        if version is not None:
            return LayerManifest(manifest, version, add_to_search_path=True)
    return None


def layer_environment(manifest: LayerManifest, log_file: Path,
                      environ: Optional[Mapping[str, str]] = None) -> Dict[str, str]:
    """The environment of an encode under the layer: the layer enabled, its
    errors and warnings written to log_file, none dropped as a repeat.

    The layer is enabled in VK_LOADER_LAYERS_ENABLE and in VK_INSTANCE_LAYERS:
    the loader reads VK_LOADER_LAYERS_ENABLE only in a process that is not
    elevated, and VK_INSTANCE_LAYERS in any, such as one started from an
    administrator's session on Windows."""
    env = dict(os.environ if environ is None else environ)
    if manifest.add_to_search_path:
        env["VK_ADD_LAYER_PATH"] = os.pathsep.join(
            filter(None, [str(manifest.path.parent), env.get("VK_ADD_LAYER_PATH")]))
    env["VK_LOADER_LAYERS_ENABLE"] = ",".join(
        filter(None, [env.get("VK_LOADER_LAYERS_ENABLE"), LAYER_NAME]))
    instance_layers = [name for name in env.get("VK_INSTANCE_LAYERS", "").split(os.pathsep) if name]
    if LAYER_NAME not in instance_layers:
        instance_layers.append(LAYER_NAME)
    env["VK_INSTANCE_LAYERS"] = os.pathsep.join(instance_layers)
    env.update({
        "VK_KHRONOS_VALIDATION_DEBUG_ACTION": "VK_DBG_LAYER_ACTION_LOG_MSG",
        "VK_KHRONOS_VALIDATION_LOG_FILENAME": str(log_file),
        "VK_KHRONOS_VALIDATION_REPORT_FLAGS": "error,warn",
        "VK_KHRONOS_VALIDATION_ENABLE_MESSAGE_LIMIT": "false",
    })
    return env


@dataclass(frozen=True)
class LayerMessage:
    """One message of the layer's log: its severity ("Error", "Warning"...),
    its VUID or other ID, and its whole text."""
    severity: str
    vuid: str
    text: str


def parse_layer_log(text: str) -> List[LayerMessage]:
    """The messages of a layer log. Each runs from its first line to the
    first blank line, or to the next message."""
    headers = list(_HEADER.finditer(text))
    messages = []
    for index, header in enumerate(headers):
        end = headers[index + 1].start() if index + 1 < len(headers) else len(text)
        body = text[header.start():end]
        blank = re.search(r"\n[ \t]*\n", body)
        messages.append(LayerMessage(header.group(1), header.group(2),
                                     body[:blank.start()] if blank else body))
    return messages


@dataclass(frozen=True)
class AllowedMessage:
    """An error of the layer an encode may report and still pass."""
    vuid: str
    reason: str
    codecs: Tuple[str, ...] = ()
    message: Optional[str] = None
    min_layer_version: Optional[Version] = None
    max_layer_version: Optional[Version] = None
    bug_url: Optional[str] = None

    @classmethod
    def from_dict(cls, data: dict) -> 'AllowedMessage':
        """An entry of the allowlist. A malformed entry raises ValueError, so
        a misspelt key cannot widen or drop it."""
        if not isinstance(data, dict):
            raise ValueError(f"allowlist entry is not an object: {data!r}")
        unknown = sorted(set(data) - _ALLOWLIST_ENTRY_KEYS)
        if unknown:
            raise ValueError(
                f"allowlist entry {data.get('vuid')!r}: unknown keys {unknown}")
        if not data.get("vuid") or not data.get("reason"):
            raise ValueError(
                f"allowlist entry {data.get('vuid')!r}: needs a vuid and a reason")
        codecs = data.get("codecs") or []
        if not isinstance(codecs, list):
            raise ValueError(
                f"allowlist entry {data['vuid']!r}: codecs is not a list")

        def version(key):
            return parse_version(data[key]) if data.get(key) is not None else None

        return cls(vuid=data["vuid"], reason=data["reason"], codecs=tuple(codecs),
                   message=data.get("message"),
                   min_layer_version=version("min_layer_version"),
                   max_layer_version=version("max_layer_version"),
                   bug_url=data.get("bug_url"))

    def allows(self, message: LayerMessage, codec: str, layer_version: Version) -> bool:
        """Whether the entry covers a message of an encode of codec, reported
        by the layer of layer_version."""
        return (fnmatch.fnmatchcase(message.vuid, self.vuid)
                and (not self.codecs or codec in self.codecs)
                and (self.message is None or self.message in message.text)
                and (self.min_layer_version is None
                     or layer_version >= self.min_layer_version)
                and (self.max_layer_version is None
                     or layer_version <= self.max_layer_version))


def load_allowlist(path: Path = DEFAULT_ALLOWLIST,
                   test_type: str = "encode") -> List[AllowedMessage]:
    """The allowlist's entries for a type of test. A malformed file raises
    ValueError."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: not an allowlist object")
    unknown = sorted(set(data) - _ALLOWLIST_FILE_KEYS)
    if unknown:
        raise ValueError(f"{path}: unknown keys {unknown}")
    entries = data.get(test_type, [])
    if not isinstance(entries, list):
        raise ValueError(f"{path}: {test_type} is not a list")
    return [AllowedMessage.from_dict(entry) for entry in entries]


def _listing(counts: Dict[str, int]) -> str:
    """VUIDs with their counts, in name order."""
    return ", ".join(f"{vuid} x{count}" for vuid, count in sorted(counts.items()))


@dataclass
class LayerReport:
    """What the layer reported for one encode, by VUID: the errors that fail
    it, the errors on the allowlist, and the messages of other severities."""
    manifest: LayerManifest
    log_file: Path
    loaded: bool
    errors: Dict[str, int] = field(default_factory=dict)
    allowed: Dict[str, int] = field(default_factory=dict)
    warnings: Dict[str, int] = field(default_factory=dict)

    @property
    def passed(self) -> bool:
        """The layer loaded, and reported no error outside the allowlist."""
        return self.loaded and not self.errors

    @property
    def failures(self) -> List[str]:
        """Why the encode fails the check."""
        if not self.loaded:
            return [f"the layer did not load: it wrote no log at {self.log_file}"]
        return [_listing(self.errors)] if self.errors else []

    def summary(self) -> str:
        """One line for the result."""
        layer = f"layer {version_text(self.manifest.api_version)}"
        if not self.loaded:
            return f"{layer} did not load: no log at {self.log_file}"
        parts = [f"errors {_listing(self.errors)}" if self.errors else "no errors"]
        if self.allowed:
            parts.append(f"allowed {_listing(self.allowed)}")
        if self.warnings:
            parts.append(f"warnings {_listing(self.warnings)}")
        return f"{layer}: " + "; ".join(parts)

    def to_dict(self) -> dict:
        """The report for the JSON results."""
        return {
            "layer_manifest": str(self.manifest.path),
            "layer_version": version_text(self.manifest.api_version),
            "log_file": str(self.log_file),
            "loaded": self.loaded,
            "errors": dict(sorted(self.errors.items())),
            "allowed": dict(sorted(self.allowed.items())),
            "warnings": dict(sorted(self.warnings.items())),
        }


def check_layer_log(log_file: Path, manifest: LayerManifest, codec: str,
                    allowlist: List[AllowedMessage]) -> LayerReport:
    """Sort the messages of an encode's layer log."""
    report = LayerReport(manifest, log_file, loaded=log_file.is_file())
    if not report.loaded:
        return report
    text = log_file.read_text(encoding="utf-8", errors="replace")
    for message in parse_layer_log(text):
        if message.severity != "Error":
            counts = report.warnings
        elif any(entry.allows(message, codec, manifest.api_version)
                 for entry in allowlist):
            counts = report.allowed
        else:
            counts = report.errors
        counts[message.vuid] = counts.get(message.vuid, 0) + 1
    return report


__all__ = [
    'AllowedMessage', 'DEFAULT_ALLOWLIST', 'LAYER_NAME', 'LayerManifest',
    'LayerMessage', 'LayerReport', 'check_layer_log', 'find_validation_layer',
    'layer_environment', 'load_allowlist', 'parse_layer_log', 'parse_version',
    'version_text',
]
