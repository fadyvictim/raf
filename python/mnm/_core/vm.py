"""Meta virtual machine and utility functions."""
# pylint: disable=no-self-use
import numpy as np
import tvm

from .. import _ffi
from .._lib import _ByteArray
from .._core.value import Value, TupleValue
from . import ndarray as _nd
from .core_utils import register_node

class Executable:
    # pylint: disable=too-many-instance-attributes
    """Meta VM executable"""

    def __init__(self, mod):
        self.mod = mod
        self._function_params = {}
        self._save = self.mod["save"]
        self._get_lib = self.mod["get_lib"]
        self._get_bytecode = self.mod["get_bytecode"]
        self._get_stats = self.mod["get_stats"]
        self._get_function_arity = self.mod["get_function_arity"]
        self._get_function_param_name = self.mod["get_function_param_name"]

    def save(self):
        """Save the Meta VM Executable.

        Returns
        -------
        code : bytearray
            The binary blob representing a serialized Relay VM executable. It
            can then be saved to disk and later deserialized into a new
            Executable.

        lib : :py:class:`~tvm.runtime.Module`
            The runtime module that contains the generated code. It is
            basically a library that is composed of hardware dependent code.

        Notes
        -----
        The returned code is organized with the following sections in order.
         - Global section. This section contains the globals used by the
         virtual machine.

         - Constant section. This section is used to store the constant pool of
         a virtual machine.

         - Primitive name section. This section is introduced to accommodate
         the list of primitive operator names that will be invoked by the
         virtual machine.

         - Code section. The VM functions, including bytecode, are sitting in
         this section.

        Examples
        --------

        .. code-block:: python

            import numpy as np
            import tvm
            from tvm import te
            from tvm import relay
            # define a simple network.
            x = relay.var('x', shape=(10, 10))
            f = relay.Function([x], x + x)
            mod = relay.Module({"main": f})
            # create a Relay VM.
            target = "llvm"
            executable = mnm._core.vm.compile(mod, target)
            code, lib = executable.save()
            # save and load the code and lib file.
            tmp = tvm.contrib.util.tempdir()
            path_lib = tmp.relpath("lib.so")
            lib.export_library(path_lib)
            with open(tmp.relpath("code.ro"), "wb") as fo:
                fo.write(code)
            loaded_lib = tvm.runtime.load_module(path_lib)
            loaded_code = bytearray(open(tmp.relpath("code.ro"), "rb").read())
            # deserialize.
            des_exec = mnm._core.vm.Executable.load_exec(loaded_code, loaded_code)
            # execute the deserialized executable.
            x_data = np.random.rand(10, 10).astype('float32')
            des_vm = mnm._core.vm.VirtualMachine(des_exec, "cpu")
            res = des_vm.run(x_data)
            print(res.numpy())
        """
        return self._save(), self._get_lib()

    @staticmethod
    def load_exec(bytecode, lib):
        """Construct an executable from saved artifacts.

        Parameters
        ----------
        bytecode : bytearray
            The binary blob representing a the Relay VM bytecode.

        lib : :py:class:`~tvm.runtime.Module`
            The runtime module that contains the generated code.

        Returns
        -------
        exec: Executable
            An executable constructed using the provided artifacts.
        """
        if isinstance(bytecode, (bytes, str)):
            bytecode = bytearray(bytecode)
        elif not isinstance(bytecode, (bytearray, _ByteArray)):
            raise TypeError("bytecode is expected to be the type of bytearray " +
                            "or TVMByteArray, but received {}".format(type(bytecode)))

        if lib is not None and not isinstance(lib, tvm.runtime.Module):
            raise TypeError("lib is expected to be the type of tvm.runtime.Module" +
                            ", but received {}".format(type(lib)))

        return Executable(_ffi.vm.Load_Executable(bytecode, lib))

    @property
    def lib(self):
        """Get the library that contains hardware dependent code.

        Returns
        -------
        ret : :py:class:`~tvm.runtime.Module`
            The runtime module that contains hardware dependent code.
        """
        return self._get_lib()

    @property
    def stats(self):
        """Get the statistics of the Relay VM executable.

        Returns
        -------
        ret : String
            The statistic information of the VM executable.
        """
        return self._get_stats()

    @property
    def primitive_ops(self):
        """Get the name of the primitive ops contained in the executable.

        Returns
        -------
        ret : List[String]
            The list of primitive ops.
        """
        ret = []
        num_primitives = _ffi.vm.GetNumOfPrimitives(self.module)
        for i in range(num_primitives):
            ret.append(_ffi.vm.GetPrimitiveFields(self.module, i))
        return ret

    @property
    def bytecode(self):
        """Get the bytecode of the Relay VM executable.

        Returns
        -------
        ret : str
            The bytecode of the executable.

        Notes
        -----
        The bytecode is in the following format:
          func_name reg_file_size num_instructions

          param1 param2 ... paramM

          instruction1

          instruction2

          ...

          instructionN

        Each instruction is printed in the following format:
          hash opcode field1 ... fieldX # The text format.

        The part starting from # is only used for visualization and debugging.
        The real serialized code doesn't contain it, therefore the deserializer
        doesn't need to deal with it as well.
        """
        return self._get_bytecode()

    @property
    def globals(self):
        """Get the globals used by the Relay VM executable.

        Returns
        -------
        ret : List[str]
            The globals contained in the executable.
        """
        ret = []
        num_globals = _ffi.vm.GetNumOfGlobals(self.module)
        for i in range(num_globals):
            ret.append(_ffi.vm.GetGlobalFields(self.module, i))
        return ret

    @property
    def module(self):
        """Return the runtime module contained in a virtual machine executable."""
        return self.mod

    def get_function_params(self, func_name):
        """Get VM Function parameters

        Parameters
        ----------
        func_name: str
            The function name.

        Returns
        -------
        ret : List[str]
            The parameter names of the function
        """
        if func_name in self._function_params:
            return self._function_params[func_name]
        arity = self._get_function_arity(func_name)
        assert arity >= 0
        params = []
        for i in range(arity):
            name = self._get_function_param_name(func_name, i)
            assert name
            params.append(name)
        self._function_params[func_name] = params
        return params


class VMCompiler:
    """Compiler that compiles Meta IRModule to Executable."""

    def __init__(self):
        self.mod = _ffi.vm.VMCompiler()
        self._lower = self.mod["lower"]
        self._get_exec = self.mod["get_executable"]
        self._set_params_func = self.mod["set_params"]
        self._get_params_func = self.mod["get_params"]
        self._optimize = self.mod["optimize"]

    def set_params(self, params):
        """Set constant parameters for the model.

        Parameters
        ----------
        params : dict of str to ndarray
            Input parameters to the graph that do not change
            during inference time. Used for constant folding.
        """
        inputs = {}
        for name, param in params.items():
            if isinstance(param, np.ndarray):
                param = _nd.array(param, device="cpu")
                inputs[name] = param
            else:
                assert isinstance(param, _nd.ndarray)
                inputs[name] = param
        self._set_params_func(inputs)

    def get_params(self):
        """Return the updated weights."""
        params = self._get_params_func()
        ret = {}
        for key, value in params.items():
            ret[key] = value.data
        return ret

    def lower(self, mod, target=None, target_host=None):
        """Lower the module to VM bytecode.

        Parameters
        ----------
        mod : Module
            The Relay module to build.

        target : str, :any:`tvm.target.Target`, or dict of str(i.e.
            device/context name) to str/tvm.target.Target, optional
            For heterogeneous compilation, it is a dictionary indicating context
            to target mapping. For homogeneous compilation, it is a build target.

        target_host : str or :any:`tvm.target.Target`, optional
            Host compilation target, if target is device.
            When TVM compiles device specific program such as CUDA,
            we also need host(CPU) side code to interact with the driver
            to setup the dimensions and parameters correctly.
            target_host is used to specify the host side codegen target.
            By default, llvm is used if it is enabled,
            otherwise a stackvm intepreter is used.
        """
        target = self._update_target(target)
        target_host = self._update_target_host(target, target_host)
        self._lower(mod, target, target_host)

    def optimize(self, mod, target=None, params=None):
        """Helper method that optimizes a Relay module via VM.

        Parameters
        ----------
        mod : Module

        target : str, :any:`tvm.target.Target`, or dict of str (i.e.
            device/context name) to str/tvm.target.Target, optional

        params : dict of str to ndarray
            Input parameters to the graph that do not change
            during inference time. Used for constant folding.

        Returns
        -------
        mod : tvm.IRModule
            The optimized relay module.

        params : dict
            The parameters of the final module.
        """
        target = self._update_target(target)
        if params:
            self.set_params(params)
        return self._optimize(mod, target), self.get_params()

    def get_exec(self):
        """Get the VM executable.

        Returns
        -------
        exec : tvm.runtime.Module
            The VM executable that contains both library code and bytecode.
        """
        return self._get_exec()

    def _update_target(self, target):
        """Update target."""
        target = target if target else tvm.target.Target.current()
        if target is None:
            raise ValueError("Target is not set in env or passed as argument.")
        tgts = {}
        if isinstance(target, (str, tvm.target.Target)):
            target = "llvm" if target == "cpu" else target
            dev_type = tvm.tir.IntImm(
                "int32", tvm.nd.device(str(target)).device_type)
            tgts[dev_type] = tvm.target.Target(target)
        elif isinstance(target, dict):
            for dev, tgt in target.items():
                tgt = "llvm" if tgt == "cpu" else tgt
                dev_type = tvm.tir.IntImm(
                    "int32", tvm.nd.device(dev).device_type)
                tgts[dev_type] = tvm.target.Target(tgt)
        else:
            raise TypeError("target is expected to be str, tvm.target.Target, " +
                            "or dict of str to str/tvm.target.Target, but received " +
                            "{}".format(type(target)))
        return tgts

    def _update_target_host(self, target, target_host):
        """Update target host."""
        target_host = None if target_host == "" else target_host
        if not target_host:
            for device_type, tgt in target.items():
                if device_type.value == tvm.nd.cpu(0).device_type:
                    target_host = tgt
                    break
        if not target_host:
            target_host = "llvm" if tvm.runtime.enabled("llvm") else "stackvm"
        if isinstance(target_host, str):
            target_host = "llvm" if target_host == "cpu" else target_host
            target_host = tvm.target.Target(target_host)
        return target_host


def compile(mod, target=None, target_host=None, params=None): #pylint: disable=redefined-builtin
    """Compile the module to VM executable. A helper function for VMCompiler.

    Parameters
    ----------
    mod : Module
        The module to build.

    target : str, :any:`tvm.target.Target`, or dict of str(i.e.
        device/context name) to str/tvm.target.Target, optional
        For heterogeneous compilation, it is a dictionary indicating context
        to target mapping. For homogeneous compilation, it is a build target.

    target_host : str or :any:`tvm.target.Target`, optional
        Host compilation target, if target is device.
        When TVM compiles device specific program such as CUDA,
        we also need host(CPU) side code to interact with the driver
        to setup the dimensions and parameters correctly.
        target_host is used to specify the host side codegen target.
        By default, llvm is used if it is enabled,
        otherwise a stackvm intepreter is used.

    params : dict of str to ndarray
        Input parameters to the graph that do not change
        during inference time. Used for constant folding.

    Returns
    -------
    exec : mnm.executor.Executable
        The VM executable that contains both library code and bytecode.
    """
    compiler = VMCompiler()
    if params:
        compiler.set_params(params)
    compiler.lower(mod, target, target_host)
    return Executable(compiler.get_exec())


# pylint: disable=protected-access
def _convert(arg):
    if isinstance(arg, np.ndarray):
        nd_arr = _nd.array(arg, device="cpu")
        return nd_arr._ndarray__value
    if isinstance(arg, _nd.ndarray):
        return arg._ndarray__value
    if isinstance(arg, (tuple, list)):
        return TupleValue([_convert(x) for x in arg])
    raise TypeError("Unsupported type: %s" % (type(arg)))


def _convert_args(args):
    cargs = [_convert(arg) for arg in args]
    return cargs


@register_node("mnm.vm.VMContext")
class VMContext(Value):
    """The VMContext holds the runtime data for an execution in the VM."""


class VirtualMachine:
    """Relay VM runtime.

    Parameters
    ----------
    exe : Executable
        The VM executable.

    device : :py:class:`Device`
        The runtime context to run the code on.

    enable_cuda_graph : bool
        Whether use CUDA graph.
    """
    def __init__(self, exe, device, enable_cuda_graph=False):
        if not isinstance(exe, Executable):
            raise TypeError("mod is expected to be the type of Executable, but received {}"
                            .format(type(exe)))
        self.module = _ffi.vm.VirtualMachine(exe.module, enable_cuda_graph)
        self._exec = exe
        self._set_devices = self.module["set_devices"]
        self._prepare_context = self.module["prepare_context"]
        self._run = self.module["run"]
        self._set_devices(device)

    def prepare_context(self, func_name, *args, **kwargs):
        """Create and initiliaze a VM Context given the name of function to invoke and arguments.

        Parameters
        ----------
        func_name : str
            The name of the function.

        args : list[mnm.ndarray] or list[np.ndarray]
            The arguments to the function.

        kwargs: dict of str to mnm.ndarray or np.ndarray
            Named arguments to the function.

        Returns
        -------
        result : VMContext
            The initialized VM context.
        """
        if kwargs:
            func_params = self._exec.get_function_params(func_name)
            new_args = [None] * len(func_params)
            assert len(args) + len(kwargs) == len(func_params)
            for k in kwargs:
                idx = func_params.index(k)
                new_args[idx] = kwargs[k]
            idx = 0
            for i, arg in enumerate(new_args):
                if arg is None:
                    new_args[i] = args[idx]
                    idx += 1
            args = new_args
        cargs = _convert_args(args)
        return self._prepare_context(func_name, *cargs)

    def run(self, *args, func_name="main", **kwargs):
        """Run the virtual machine.

        Parameters
        ----------
        args : list[mnm.ndarray] or list[np.ndarray]
            The arguments to the function.

        func_name : str
            The name of function to run.

        kwargs: dict of str to mnm.ndarray or np.ndarray
            Named arguments to the function.

        Returns
        -------
        result : Object
            The output.
        """
        ctx = self.prepare_context(func_name, *args, **kwargs)
        return self._run(ctx)