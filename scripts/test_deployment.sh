#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${URAN_DEPLOY_TEST_SDK:?Set URAN_DEPLOY_TEST_SDK to an installed framework SDK}"
: "${URAN_DEPLOY_TEST_PYTHON_SITE:?Set URAN_DEPLOY_TEST_PYTHON_SITE to a target-compatible installed dependency directory}"
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 "${URAN_PYTHON:-python3}" -m pytest -q "$repo_dir/tests/deployment"
