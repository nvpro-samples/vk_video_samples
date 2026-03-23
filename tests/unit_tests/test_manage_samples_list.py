#!/usr/bin/env python3
"""
Unit tests for manage_samples_list.py

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

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from io import StringIO

from tests.manage_samples_list import (
    normalize_test_name,
    load_skip_list,
    save_skip_list,
    load_sample_list,
    save_sample_list,
    validate_skip_list,
    VALID_TEST_TYPES,
    VALID_FORMATS,
    VALID_DRIVERS,
    VALID_PLATFORMS,
    VALID_REPRODUCTION,
)


class TestNormalizeTestName(unittest.TestCase):
    """Tests for normalize_test_name function"""

    def test_decode_prefix_removed(self):
        """Test that decode_ prefix is removed"""
        self.assertEqual(
            normalize_test_name("decode_av1_filmgrain_8bit"),
            "av1_filmgrain_8bit"
        )

    def test_encode_prefix_removed(self):
        """Test that encode_ prefix is removed"""
        self.assertEqual(
            normalize_test_name("encode_h264_cbr_ratecontrol"),
            "h264_cbr_ratecontrol"
        )

    def test_no_prefix_unchanged(self):
        """Test that names without prefix are unchanged"""
        self.assertEqual(
            normalize_test_name("av1_filmgrain_8bit"),
            "av1_filmgrain_8bit"
        )

    def test_empty_string(self):
        """Test empty string handling"""
        self.assertEqual(normalize_test_name(""), "")

    def test_partial_prefix_not_removed(self):
        """Test that partial prefixes are not removed"""
        self.assertEqual(normalize_test_name("decode"), "decode")
        self.assertEqual(normalize_test_name("decod_test"), "decod_test")
        self.assertEqual(normalize_test_name("encoder_test"), "encoder_test")

    def test_case_sensitive(self):
        """Test that prefix matching is case-sensitive"""
        self.assertEqual(
            normalize_test_name("DECODE_test"),
            "DECODE_test"
        )
        self.assertEqual(
            normalize_test_name("Decode_test"),
            "Decode_test"
        )


class TestLoadSkipList(unittest.TestCase):
    """Tests for load_skip_list function"""

    def test_load_existing_file(self):
        """Test loading an existing skip list file"""
        with tempfile.NamedTemporaryFile(
                mode='w', suffix='.json', delete=False) as f:
            test_data = {
                "version": "1.0",
                "description": "Test skip list",
                "decode": [
                    {"name": "test1", "format": "vvs"}
                ],
                "encode": []
            }
            json.dump(test_data, f)
            f.flush()

            result = load_skip_list(Path(f.name))
            self.assertEqual(result, test_data)

            # Cleanup
            Path(f.name).unlink()

    def test_load_nonexistent_file(self):
        """Test loading a non-existent file creates default structure"""
        with patch('sys.stdout', new_callable=StringIO):
            result = load_skip_list(Path("/nonexistent/path.json"))

        self.assertIn("version", result)
        self.assertIn("decode", result)
        self.assertIn("encode", result)
        self.assertEqual(result["decode"], [])
        self.assertEqual(result["encode"], [])

    def test_load_invalid_json(self):
        """Test loading invalid JSON exits with error"""
        with tempfile.NamedTemporaryFile(
                mode='w', suffix='.json', delete=False) as f:
            f.write("{ invalid json }")
            f.flush()

            with self.assertRaises(SystemExit):
                load_skip_list(Path(f.name))

            # Cleanup
            Path(f.name).unlink()


class TestSaveSkipList(unittest.TestCase):
    """Tests for save_skip_list function"""

    def test_save_skip_list(self):
        """Test saving a skip list to file"""
        test_data = {
            "version": "1.0",
            "skipped_tests": [
                {"name": "test1", "type": "decode"}
            ]
        }

        with tempfile.NamedTemporaryFile(
                mode='w', suffix='.json', delete=False) as f:
            temp_path = Path(f.name)

        with patch('sys.stdout', new_callable=StringIO):
            save_skip_list(temp_path, test_data)

        # Verify file contents
        with open(temp_path, 'r', encoding='utf-8') as f:
            saved_data = json.load(f)

        self.assertEqual(saved_data, test_data)

        # Verify trailing newline
        with open(temp_path, 'r', encoding='utf-8') as f:
            content = f.read()
        self.assertTrue(content.endswith('\n'))

        # Cleanup
        temp_path.unlink()


class TestLoadSampleList(unittest.TestCase):
    """Tests for load_sample_list function"""

    def test_load_existing_file(self):
        """Test loading an existing sample list file"""
        with tempfile.NamedTemporaryFile(
                mode='w', suffix='.json', delete=False) as f:
            test_data = {
                "samples": [
                    {"name": "sample1", "codec": "h264"}
                ]
            }
            json.dump(test_data, f)
            f.flush()

            result = load_sample_list(Path(f.name), "decode")
            self.assertEqual(result, test_data)

            # Cleanup
            Path(f.name).unlink()

    def test_load_nonexistent_file(self):
        """Test loading a non-existent file creates default structure"""
        with patch('sys.stdout', new_callable=StringIO):
            result = load_sample_list(Path("/nonexistent/path.json"), "decode")

        self.assertIn("samples", result)
        self.assertEqual(result["samples"], [])


class TestSaveSampleList(unittest.TestCase):
    """Tests for save_sample_list function"""

    def test_save_sample_list(self):
        """Test saving a sample list to file"""
        test_data = {
            "samples": [
                {"name": "sample1", "codec": "h264"}
            ]
        }

        with tempfile.NamedTemporaryFile(
                mode='w', suffix='.json', delete=False) as f:
            temp_path = Path(f.name)

        with patch('sys.stdout', new_callable=StringIO):
            save_sample_list(temp_path, test_data, "decode")

        # Verify file contents
        with open(temp_path, 'r', encoding='utf-8') as f:
            saved_data = json.load(f)

        self.assertEqual(saved_data, test_data)

        # Cleanup
        temp_path.unlink()


class TestValidateSkipList(unittest.TestCase):
    """Tests for validate_skip_list function"""

    def test_valid_skip_list(self):
        """Test validation of a valid skip list"""
        skip_list = {
            "decode": [
                {
                    "name": "test1",
                    "format": "vvs",
                    "drivers": ["all"],
                    "platforms": ["all"],
                    "reproduction": "always",
                    "reason": "Test reason",
                    "date_added": "2025-01-01"
                }
            ],
            "encode": []
        }

        with patch('sys.stdout', new_callable=StringIO):
            result = validate_skip_list(skip_list)

        self.assertTrue(result)

    def test_missing_sections(self):
        """Test validation fails without decode/encode sections"""
        skip_list = {"version": "1.0"}

        with patch('sys.stdout', new_callable=StringIO):
            result = validate_skip_list(skip_list)

        self.assertFalse(result)

    def test_missing_name_field(self):
        """Test validation fails for entry without name"""
        skip_list = {
            "decode": [
                {"format": "vvs"}
            ],
            "encode": []
        }

        with patch('sys.stdout', new_callable=StringIO):
            result = validate_skip_list(skip_list)

        self.assertFalse(result)

    def test_invalid_format_value(self):
        """Test validation fails for invalid format value"""
        skip_list = {
            "decode": [
                {"name": "test1", "format": "invalid"}
            ],
            "encode": []
        }

        with patch('sys.stdout', new_callable=StringIO):
            result = validate_skip_list(skip_list)

        self.assertFalse(result)

    def test_invalid_driver_value(self):
        """Test validation fails for invalid driver value"""
        skip_list = {
            "decode": [
                {
                    "name": "test1",
                    "format": "vvs",
                    "drivers": ["invalid_driver"]
                }
            ],
            "encode": []
        }

        with patch('sys.stdout', new_callable=StringIO):
            result = validate_skip_list(skip_list)

        self.assertFalse(result)

    def test_invalid_platform_value(self):
        """Test validation fails for invalid platform value"""
        skip_list = {
            "decode": [
                {
                    "name": "test1",
                    "format": "vvs",
                    "platforms": ["invalid_platform"]
                }
            ],
            "encode": []
        }

        with patch('sys.stdout', new_callable=StringIO):
            result = validate_skip_list(skip_list)

        self.assertFalse(result)

    def test_invalid_reproduction_value(self):
        """Test validation fails for invalid reproduction value"""
        skip_list = {
            "decode": [
                {
                    "name": "test1",
                    "format": "vvs",
                    "reproduction": "invalid"
                }
            ],
            "encode": []
        }

        with patch('sys.stdout', new_callable=StringIO):
            result = validate_skip_list(skip_list)

        self.assertFalse(result)

    def test_warning_missing_reason(self):
        """Test validation warns for missing reason"""
        skip_list = {
            "decode": [
                {
                    "name": "test1",
                    "format": "vvs",
                    "date_added": "2025-01-01"
                }
            ],
            "encode": []
        }

        output = StringIO()
        with patch('sys.stdout', output):
            result = validate_skip_list(skip_list)

        # Should pass with warnings
        self.assertTrue(result)
        self.assertIn("WARNING", output.getvalue())

    def test_warning_missing_date_added(self):
        """Test validation warns for missing date_added"""
        skip_list = {
            "decode": [
                {
                    "name": "test1",
                    "format": "vvs",
                    "reason": "Test reason"
                }
            ],
            "encode": []
        }

        output = StringIO()
        with patch('sys.stdout', output):
            result = validate_skip_list(skip_list)

        # Should pass with warnings
        self.assertTrue(result)
        self.assertIn("WARNING", output.getvalue())

    def test_empty_skip_list(self):
        """Test validation of empty skip list"""
        skip_list = {"decode": [], "encode": []}

        with patch('sys.stdout', new_callable=StringIO):
            result = validate_skip_list(skip_list)

        self.assertTrue(result)


class TestValidConstants(unittest.TestCase):
    """Tests for valid constant values"""

    def test_valid_test_types(self):
        """Test valid test types are defined"""
        self.assertIn("decode", VALID_TEST_TYPES)
        self.assertIn("encode", VALID_TEST_TYPES)

    def test_valid_formats(self):
        """Test valid formats are defined"""
        self.assertIn("vvs", VALID_FORMATS)
        self.assertIn("fluster", VALID_FORMATS)
        self.assertIn("soothe", VALID_FORMATS)

    def test_valid_drivers(self):
        """Test valid drivers are defined"""
        self.assertIn("all", VALID_DRIVERS)
        self.assertIn("nvidia", VALID_DRIVERS)
        self.assertIn("nvk", VALID_DRIVERS)
        self.assertIn("amd", VALID_DRIVERS)
        self.assertIn("radv", VALID_DRIVERS)
        self.assertIn("intel", VALID_DRIVERS)
        self.assertIn("anv", VALID_DRIVERS)

    def test_valid_platforms(self):
        """Test valid platforms are defined"""
        self.assertIn("all", VALID_PLATFORMS)
        self.assertIn("linux", VALID_PLATFORMS)
        self.assertIn("windows", VALID_PLATFORMS)

    def test_valid_reproduction(self):
        """Test valid reproduction values are defined"""
        self.assertIn("always", VALID_REPRODUCTION)
        self.assertIn("flaky", VALID_REPRODUCTION)


if __name__ == "__main__":
    unittest.main()
