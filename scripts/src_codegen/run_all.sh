#!/bin/bash
set -e
set -u
set -o pipefail

python3 scripts/src_codegen/main_cxx_schema.py
python3 scripts/src_codegen/main_cxx_reg.py
python3 scripts/src_codegen/main_py_sym.py
python3 scripts/src_codegen/main_py_ffi.py
