from numbers import Number

import def_op
from codegen_utils import write_to_file, NORM_MAP


def gen_file():
    FILE = """
import mnm._ffi.op.sym as ffi
from mnm._core.ndarray import Symbol
from . import sym_utils
{METHODS}
""".strip()
    ops = def_op.by_name()
    methods = "\n".join(gen_method(ops[name])
                        for name in sorted(ops.keys()))
    return FILE.format(METHODS=methods)


def gen_method(op):
    METHOD = """
def {NAME}({PARAMS_W_DEFAULT}):
{NORMS}
    return Symbol.from_expr(ffi.{NAME}({PARAMS_WO_DEFAULT}))
""".strip()
    name = op.name
    norms = "\n".join(map(gen_norm, op.schema))
    param_w = gen_param_w_default(op.schema)
    param_wo = gen_param_wo_default(op.schema)
    return METHOD.format(NAME=name,
                         NORMS=norms,
                         PARAMS_W_DEFAULT=param_w,
                         PARAMS_WO_DEFAULT=param_wo)


def gen_norm(entry):
    NORM = " " * 4 + """
    {NAME} = sym_utils.{NORM}({NAME})
""".strip()
    name = entry.name
    norm = NORM_MAP[entry.py_normalizer or (entry.cxx_normalizer or entry.cxx_type)]
    return NORM.format(NAME=name, NORM=norm)


def gen_param_w_default(schema):
    result = []
    for entry in schema:
        name = entry.name
        # case 1: no default value
        if entry.cxx_default is None:
            result.append(name)
            continue
        # case 2: python-specific default value is provided
        if entry.py_default is not None:
            default = entry.py_default
        # case 3: no python-specific default is provided
        elif entry.cxx_default == "nullptr":
            default = None
        elif isinstance(entry.cxx_default, Number):
            default = entry.cxx_default
        else:
            raise NotImplementedError(entry)
        result.append(f"{name}={default}")
    return ", " .join(result)


def gen_param_wo_default(schema):
    return ", ".join(arg.name for arg in schema)


def main(path="./python/mnm/op/sym/sym.py"):
    result = gen_file()
    write_to_file(path, result)


if __name__ == "__main__":
    main()
