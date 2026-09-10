"""
Shared minimal framework stand-in for the unit tests.

VulkanVideoTestFrameworkBase is abstract, so any test that exercises its
concrete logic -- status determination, result scoring -- needs a subclass that
fills in the abstract methods and nothing else. This is that subclass, kept in
one place so every test module shares it instead of carrying its own copy.

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

from tests.libs.video_test_config_base import SkipFilter
from tests.libs.video_test_framework_base import VulkanVideoTestFrameworkBase


class MockFramework(VulkanVideoTestFrameworkBase):
    """Concrete framework with no executable, no resources and no test suite.

    Everything the base class needs to be instantiable is stubbed out, so a test
    can call the real method under test without a binary, a GPU or a sample file.
    """

    def __init__(self):
        super().__init__(executable_path=None)
        self._skip_rules = []
        self._options['skip_filter'] = SkipFilter.ENABLED

    def check_resources(self, _auto_download=True, _test_configs=None):
        """No resources are needed; report them as present."""
        return True

    def create_test_suite(self):
        """No suite is built; the tests supply their own configs."""
        return []

    def run_single_test(self, _config):
        """Nothing is executed; the tests call the scoring methods directly."""
