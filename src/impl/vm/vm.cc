/*!
 * Copyright (c) 2020 by Contributors
 * \file src/impl/vm/vm.cc
 * \brief The Meta virtual machine.
 */

#include <dmlc/memory_io.h>
#include <tvm/runtime/memory.h>
#include <tvm/runtime/object.h>
#include <tvm/runtime/device_api.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "mnm/memory_pool.h"
#include "mnm/ir.h"
#include "mnm/op.h"
#include "mnm/op_utils.h"
#include "mnm/value.h"
#include "mnm/type.h"
#include "mnm/pass.h"
#include "mnm/vm/bytecode.h"
#include "mnm/vm/vm.h"
#include "mnm/device_api.h"
#include "mnm/profiler.h"
#include "../../requests.h"
#include "../../op/ty/utils.h"
#include "../../common/shape_utils.h"

#include "mnm/device_api.h"
#include "mnm/registry.h"

#ifdef MNM_USE_CUDA
#include "../../common/cuda_utils.h"
#include "../../op/dialect/cudnn/cudnn_utils.h"
#include "../../op/dialect/cublas/cublas_utils.h"
#include "../../profiler/cuda/cuda_profiler.h"
#endif

namespace mnm {
namespace executor {
namespace vm {

using namespace mnm::ir;
using namespace mnm::value;
using namespace mnm::op;
using namespace mnm::registry;
using namespace mnm::requests;
using namespace mnm::device_api;

MNM_REGISTER_OBJECT_REFLECT(VMContextObj);

VMContext VMContext::make(const Executable* exec) {
  auto ptr = make_object<VMContextObj>();
  ptr->exec = exec;
  return VMContext(ptr);
}

inline Value VMContext::ReadRegister(Index reg) const {
  auto self = this->operator->();
  return self->frames.back().register_file[reg];
}

inline void VMContext::WriteRegister(Index reg, const Value& val) {
  auto self = this->operator->();
  self->frames.back().register_file[reg] = val;
}

inline int64_t VMContext::LoadScalarInt(Index r) const {
  int32_t result;
  const auto& obj = ReadRegister(r);
  auto int_value = Downcast<IntValue>(obj);
  return int_value->value;
}

inline bool VMContext::IsConst(Index reg) const {
  auto self = this->operator->();
  return self->frames.back().is_const[reg];
}

inline void VMContext::PushFrame(Index func_index, const std::vector<Value>& args,
                                 RegName ret_reg) {
  auto self = this->operator->();
  const auto& func = self->exec->functions[func_index];
  CHECK_EQ(func.params.size(), args.size())
      << "Number of arguments mismatches: " << func.params.size() << " vs " << args.size();
  auto ret_pc = self->pc + 1;
  auto frame = VMFrame(self->func_index, ret_pc, ret_reg, args.size(), func.register_file_size);
  self->frames.push_back(frame);
  for (size_t i = 0; i < args.size(); ++i) {
    WriteRegister(i, args[i]);
  }
  self->func_index = func_index;
  self->code = func.instructions.data();
  self->pc = 0;
}

inline Index VMContext::PopFrame() {
  auto self = this->operator->();
  CHECK_GT(self->frames.size(), 0);
  const VMFrame& fr = self->frames.back();
  self->func_index = fr.caller_func_index;
  self->pc = fr.caller_return_pc;
  self->code = self->exec->functions[self->func_index].instructions.data();
  self->frames.pop_back();
  return fr.caller_return_register;
}

std::shared_ptr<OpEnvCache> VMFuncOpEnvCache::Get(Index pc) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = cache_map_.find(pc);
  if (it != cache_map_.end()) {
    return it->second;
  }
  auto cache = std::make_shared<OpEnvCache>();
  cache_map_.emplace(pc, cache);
  return cache;
}

void VMFuncOpEnvCache::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  cache_map_.clear();
}

#ifdef MNM_USE_CUDA
void MNMSetStream(Device dev, cudaStream_t stream) {
  tvm::runtime::DeviceAPI::Get(dev)->SetStream(dev, stream);
  mnm::op::cudnn::SetStream(stream);
  mnm::op::cublas::SetStream(stream);
}

class VirtualMachine::CudaGraphImpl {
 public:
  CudaGraphImpl(Device dev) : device_(dev) {
    DLOG(INFO) << "Use Cuda Graph";
  }

  ~CudaGraphImpl() {
    CUDA_CALL(cudaGraphDestroy(graph_));
    CUDA_CALL(cudaGraphExecDestroy(exec_));
    CUDA_CALL(cudaStreamDestroy(stream_for_graph_));
  }

  void GetKernelInfo() {
    cudaGraphNode_t* nodes = NULL;
    size_t numNodes = 0;
    CUDA_CALL(cudaGraphGetNodes(graph_, nodes, &numNodes));
    cudaKernelNodeParams* pNodeParams;
    DLOG(INFO) << "Num of nodes in captured graph: " << (numNodes);
    CHECK_GT(numNodes, 0) << "Generated CUDA Graph is empty";
  }

  void BeginCapture() {
    stream_for_graph_ = static_cast<cudaStream_t>(
        mnm::device_api::DeviceAPI::Get(device_.device_type)->CreateStream(device_));

    MNMSetStream(device_, stream_for_graph_);
    CUDA_CALL(cudaStreamBeginCapture(stream_for_graph_, cudaStreamCaptureModeRelaxed));
  }

  void EndCapture() {
    CUDA_CALL(cudaStreamEndCapture(stream_for_graph_, &graph_));
    CUDA_CALL(cudaGraphInstantiate(&exec_, graph_, NULL, NULL, 0));
    GetKernelInfo();
    is_captured_ = true;
  }

  void Invoke() {
    CUDA_CALL(cudaGraphLaunch(exec_, NULL));
    CUDA_CALL(cudaStreamSynchronize(stream_for_graph_));
  }

 private:
  bool is_captured_ = false;
  cudaStream_t stream_for_graph_;
  cudaGraph_t graph_;
  cudaGraphExec_t exec_;
  Device device_;
};
#endif

VirtualMachine::~VirtualMachine() {
#ifdef MNM_USE_CUDA
  if (!devices_.empty()) MNMSetStream(devices_[0], 0);
  for (auto stream : cuda_streams_) CUDA_CALL(cudaStreamDestroy(stream));
  for (auto event : cuda_events_) CUDA_CALL(cudaEventDestroy(event));
#endif
}

PackedFunc VirtualMachine::GetFunction(const std::string& name,
                                       const ObjectPtr<Object>& sptr_to_self) {
  if (name == "run") {
    return PackedFunc([sptr_to_self, this](registry::TVMArgs args, registry::TVMRetValue* rv) {
      VMContext ctx = args[0];
      *rv = Run(ctx);
    });
  } else if (name == "set_devices") {
    return PackedFunc([sptr_to_self, this](registry::TVMArgs args, registry::TVMRetValue* rv) {
      std::vector<Device> devices;
      for (int i = 0; i < args.size(); ++i) {
        DLDevice dev = args[i];
        devices.push_back(dev);
      }
      this->SetDevices(devices);
    });
  } else if (name == "prepare_context") {
    return PackedFunc([sptr_to_self, this](registry::TVMArgs args, registry::TVMRetValue* rv) {
      CHECK(exec_) << "The executable is not loaded yet.";
      std::string func_name = args[0];
      std::vector<Value> inputs(args.size() - 1);
      for (size_t i = 1; i < args.size(); ++i) {
        inputs[i - 1] = args[i];
      }
      *rv = PrepareVMContext(func_name, inputs);
    });
  } else {
    LOG(FATAL) << "Unknown packed function: " << name;
    return PackedFunc([sptr_to_self, name](registry::TVMArgs args, registry::TVMRetValue* rv) {});
  }
}

void VirtualMachine::LoadExecutable(const Executable* exec) {
  CHECK(exec) << "The executable is not created yet.";
  exec_ = exec;
  for (int i = 0; i < exec_->functions.size(); ++i) {
    op_env_cache_.push_back(std::make_shared<VMFuncOpEnvCache>());
  }

  tvm::runtime::Module lib = exec_->lib;
  // Get the list of packed functions.
  CHECK(exec->primitive_map.empty() || lib.operator->())
      << "runtime module should have been built for primitive functions"
      << "\n";
  for (const auto& it : exec_->primitive_map) {
    const auto& packed_name = it.first;
    auto packed_index = static_cast<size_t>(it.second);
    if (packed_funcs_.size() <= packed_index) {
      packed_funcs_.resize(packed_index + 1);
    }
    tvm::runtime::PackedFunc pf = lib.GetFunction(packed_name, true);
    CHECK(pf != nullptr) << "Cannot find function in module: " << packed_name;
    packed_funcs_[packed_index] = pf;
  }
}

VMContext VirtualMachine::PrepareVMContext(const std::string& func_name,
                                           const std::vector<Value>& inputs) {
  auto gvit = exec_->global_map.find(func_name);
  CHECK(gvit != exec_->global_map.end()) << "Cannot find function " << func_name;
  auto func_index = gvit->second;
  const auto& vm_func = exec_->functions[func_index];
  CHECK_EQ(inputs.size(), vm_func.params.size())
      << "The number of inputs doesn't match the number of parameters for function " << func_name;

  auto fcreate_ctx = [&]() {
    auto ctx = VMContext::make(exec_);
    ctx->entry_func_index = func_index;
    ctx->inputs.resize(inputs.size());
    // TODO(@zhiics, @icemelon9): For heterogeneous execution, get input device information
    Device dev = devices_[0];
    for (size_t i = 0; i < inputs.size(); ++i) {
      ctx->inputs[i] = CopyTo(inputs[i], dev);
    }
    return ctx;
  };
#ifdef MNM_USE_CUDA
  if (enable_cuda_graph_) {
    std::lock_guard<std::mutex> lock(cuda_graph_mutex_);
    // Check if there is another context using the CUDA graph
    CHECK(!cuda_graph_occupied_) << "VM in CUDA graph mode doesn't support concurrent execution";
    if (!cuda_graph_ctx_.defined() || cuda_graph_ctx_->entry_func_index != func_index) {
      // Initialize the cuda graph context for the first time, or reset the cuda graph context
      // because this time invokes a different function
      cuda_graph_impl_ = nullptr;
      cuda_graph_ctx_ = fcreate_ctx();
    } else {
      for (int i = 0; i < inputs.size(); i++) {
        Value new_arg = inputs[i];
        Value graph_arg = cuda_graph_ctx_->inputs[i];
        if (new_arg.as<TensorValueObj>()) {
          CHECK(graph_arg.as<TensorValueObj>()) << "Value type mismatch, cannot copy";
          Downcast<TensorValue>(new_arg)->tensor.CopyTo(Downcast<TensorValue>(graph_arg)->tensor);
        } else {
          LOG(FATAL) << "Unsupported Value Type for reusing CUDA Graph";
        }
      }
      DLOG(INFO) << "Updated the inputs to the cached CUDA Graph.";
    }
    cuda_graph_occupied_ = true;
    return cuda_graph_ctx_;
  }
#endif
  auto ctx = fcreate_ctx();
  return ctx;
}

Value VirtualMachine::Run(VMContext ctx) {
  auto frun = [&]() {
    // ctx->pc will be reset to 0 in the PushFrame
    ctx.PushFrame(ctx->entry_func_index, ctx->inputs, -1);
    RunLoop(ctx);
  };
#ifdef MNM_USE_CUDA
  if (enable_cuda_graph_) {
    CHECK(ctx.get() == cuda_graph_ctx_.get()) << "Wrong VMContext provided for CUDA graph.";
    if (!cuda_graph_impl_) {
      cuda_graph_impl_ = new CudaGraphImpl(devices_[0]);
      DLOG(INFO) << "Begin capturing CUDA graph.";
      cuda_graph_impl_->BeginCapture();
      frun();
      cuda_graph_impl_->EndCapture();
      DLOG(INFO) << "CUDA graph captured.";
    }
    cuda_graph_impl_->Invoke();
    std::lock_guard<std::mutex> lock(cuda_graph_mutex_);
    cuda_graph_occupied_ = false;
    // TODO(@icemelon9, @zhiics): May need to copy the return register to the host device to
    // avoid data race
    return ctx->return_register;
  }
#endif
  frun();
  return ctx->return_register;
}

Device VirtualMachine::GetParamsDevice() const {
  CHECK(!devices_.empty()) << "Devices have not been initialized yet.";

  // Use the fallback device if no device index is available.
  int fallback_device_type = static_cast<int>(devices_[0].device_type);
  // TODO(@zhiics): For heterogeneous execution, get device information from byte

  const auto& cit =
      std::find_if(devices_.begin(), devices_.end(), [&fallback_device_type](const Device& d) {
        return fallback_device_type == static_cast<int>(d.device_type);
      });
  return (cit == devices_.end() ? devices_[0] : *cit);
}

void VirtualMachine::SetDevices(const std::vector<Device>& devices) {
  devices_ = devices;
  use_cuda_ = false;
  for (const Device& dev : devices) {
    if (dev.device_type == DevType::kCUDA()) {
      use_cuda_ = true;
    }
    if (dev.device_type == DevType::kCPU()) {
      host_device_ = dev;
    }
  }
  if (host_device_.device_id < 0) {
    host_device_ = Device(DevType::kCPU(), 0);
  }
  if (!use_cuda_) {
    enable_cuda_graph_ = false;
  }
}

void VirtualMachine::RunLoop(VMContext& ctx) {
  CHECK(this->exec_);
  CHECK_GT(ctx->frames.size(), 0) << "The call stack is empty";
  CHECK(ctx->code);
#ifdef MNM_USE_CUDA
  if (use_cuda_ && profiler::Profiler::Get()->IsProfiling(1)) {
    profiler::CudaProfiler::Get()->start();
  }
#endif
  while (true) {
  main_loop:
    auto const& instr = ctx->code[ctx->pc];
    switch (instr.op) {
      case Opcode::Move: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "Move", "VMInstruction", {},
                                 { HandleMove(ctx, instr); });
        goto main_loop;
      }
      case Opcode::Fatal: {
        throw std::runtime_error("VM encountered fatal error");
      }
      case Opcode::LoadConst: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "LoadConst", "VMInstruction", {},
                                 { HandleLoadConst(ctx, instr); });
        goto main_loop;
      }
      case Opcode::LoadConsti: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "LoadConsti", "VMInstruction", {},
                                 { HandleLoadConsti(ctx, instr); });
        goto main_loop;
      }
      case Opcode::GetField: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "GetField", "VMInstruction", {},
                                 { HandleGetField(ctx, instr); });
        goto main_loop;
      }
      case Opcode::Goto: {
        ctx->pc += instr.pc_offset;
        goto main_loop;
      }
      case Opcode::If: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "If", "VMInstruction", {},
                                 { HandleIf(ctx, instr); });
        goto main_loop;
      }
      case Opcode::AllocStorage: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "AllocStorage", "VMInstruction", {},
                                 { HandleAllocStorage(ctx, instr); });
        goto main_loop;
      }
      case Opcode::AllocTensor: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "AllocTensor", "VMInstruction", {},
                                 { HandleAllocTensor(ctx, instr); });
        goto main_loop;
      }
      case Opcode::AllocTensorReg: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "AllocTensorReg", "VMInstruction", {},
                                 { HandleAllocTensorReg(ctx, instr); });
        goto main_loop;
      }
      case Opcode::AllocTuple: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "AllocTuple", "VMInstruction", {},
                                 { HandleAllocTuple(ctx, instr); });
        goto main_loop;
      }
      case Opcode::AllocClosure: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "AllocClosure", "VMInstruction", {},
                                 { HandleAllocClosure(ctx, instr); });
        goto main_loop;
      }
      case Opcode::Free: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "Free", "VMInstruction", {},
                                 { HandleFree(ctx, instr); });
        goto main_loop;
      }
      case Opcode::SetShape: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "SetShape", "VMInstruction", {},
                                 { HandleSetShape(ctx, instr); });
        goto main_loop;
      }
      case Opcode::InvokeFunc: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "InvokeFunc", "VMInstruction", {},
                                 { HandleInvokeFunc(ctx, instr); });
        goto main_loop;
      }
      case Opcode::InvokePacked: {
        LOG(FATAL) << "Not supported.";
      }
      case Opcode::InvokeClosure: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "InvokeClosure", "VMInstruction", {},
                                 { HandleInvokeClosure(ctx, instr); });
        goto main_loop;
      }
      case Opcode::InvokeJit: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "InvokeJit", "VMInstruction", {},
                                 { HandleInvokeJit(ctx, instr); });
        goto main_loop;
      }
      case Opcode::InferType: {
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "InferType", "VMInstruction", {},
                                 { HandleInferType(ctx, instr); });
        goto main_loop;
      }
      case Opcode::Ret: {
        bool final_ret;
        WITH_BASE_PROFILER_LEVEL(2, host_device_, "Ret", "VMInstruction", {},
                                 { final_ret = HandleRet(ctx, instr); });
        if (final_ret) {
          return;
        }
        goto main_loop;
      }
      case Opcode::CudaSetStream: {
        HandleCudaSetStream(ctx, instr);
        goto main_loop;
      }
      case Opcode::CudaAddEvent: {
        HandleCudaAddEvent(ctx, instr);
        goto main_loop;
      }
      case Opcode::CudaWaitEvent: {
        HandleCudaWaitEvent(ctx, instr);
        goto main_loop;
      }
    }
  }
}

void VirtualMachine::HandleMove(VMContext& ctx, const Instruction& instr) {
  Value from_obj = ctx.ReadRegister(instr.from);
  ctx.WriteRegister(instr.dst, from_obj);
  ctx->pc++;
}

void VirtualMachine::HandleLoadConst(VMContext& ctx, const Instruction& instr) {
  auto constant_obj = exec_->constants[instr.const_index];
  // We cache the allocated object in the constant pool. To measure, the
  // first iteration will set the pool up. The other iterations will
  // directly reuse the allocated objects.
  if (const_pool_.size() <= static_cast<size_t>(instr.const_index)) {
    const_pool_.resize(instr.const_index + 1);
  }

  if (!const_pool_[instr.const_index].defined()) {
    // TODO(@zhiics): device could be obtained from the device list.
    const_pool_[instr.const_index] = CopyTo(constant_obj, devices_[0]);
  }
  ctx.WriteRegister(instr.dst, const_pool_[instr.const_index]);
  ctx->frames.back().is_const[instr.dst] = true;
  ctx->pc++;
}

void VirtualMachine::HandleLoadConsti(VMContext& ctx, const Instruction& instr) {
  ctx.WriteRegister(instr.dst, ScalarValue::make(instr.load_consti.val));
  ctx->pc++;
}

void VirtualMachine::HandleGetField(VMContext& ctx, const Instruction& instr) {
  auto object = ctx.ReadRegister(instr.get_field.object);
  const auto& tuple = Downcast<TupleValue>(object);
  auto field = tuple->fields[instr.get_field.field_index];
  ctx.WriteRegister(instr.dst, field);
  ctx->pc++;
}

void VirtualMachine::HandleIf(VMContext& ctx, const Instruction& instr) {
  int32_t test_val = ctx.LoadScalarInt(instr.if_op.test);
  int32_t target_val = ctx.LoadScalarInt(instr.if_op.target);

  if (test_val == target_val) {
    CHECK_NE(instr.if_op.true_offset, 0);
    ctx->pc += instr.if_op.true_offset;
  } else {
    CHECK_NE(instr.if_op.false_offset, 0);
    ctx->pc += instr.if_op.false_offset;
  }
}

void VirtualMachine::HandleAllocStorage(VMContext& ctx, const Instruction& instr) {
  auto size = ctx.LoadScalarInt(instr.alloc_storage.allocation_size);
  auto alignment = instr.alloc_storage.alignment;

  DLOG(INFO) << "AllocStorage: allocation_size=" << size << " alignment=" << alignment
             << " dtype_hint=" << tvm::runtime::DLDataType2String(instr.alloc_storage.dtype_hint);

  auto dev = Device(instr.alloc_storage.device_type, instr.alloc_storage.device_id);
  auto buffer = memory_pool::Memory::Alloc(dev, size, alignment);
  auto storage = StorageValue::make(buffer);
  ctx.WriteRegister(instr.dst, storage);
  ctx->pc++;
}

void VirtualMachine::HandleAllocTensor(VMContext& ctx, const Instruction& instr) {
  auto shape = std::vector<int64_t>(instr.alloc_tensor.ndim);
  for (uint32_t i = 0; i < instr.alloc_tensor.ndim; ++i) {
    shape[i] = instr.alloc_tensor.shape[i];
  }

  auto storage_obj = ctx.ReadRegister(instr.alloc_tensor.storage);
  auto storage = Downcast<StorageValue>(storage_obj);
  std::shared_ptr<memory_pool::Memory> mem = nullptr;
  if (instr.alloc_tensor.own) {
    mem = storage->buffer;
  }
  auto tensor = TensorValue::Assemble(storage->buffer->device, instr.alloc_tensor.dtype, shape, {},
                                      storage->buffer->data, mem);
  ctx.WriteRegister(instr.dst, tensor);
  ctx->pc++;
}

void VirtualMachine::HandleAllocTensorReg(VMContext& ctx, const Instruction& instr) {
  Value value = ctx.ReadRegister(instr.alloc_tensor_reg.shape_register);
  const auto* tuple = value.as<TupleValueObj>();
  auto shape = std::vector<int64_t>(tuple->fields.size());
  for (size_t i = 0; i < tuple->fields.size(); ++i) {
    shape[i] = Downcast<IntValue>(tuple->fields[i])->value;
  }

  auto storage_obj = ctx.ReadRegister(instr.alloc_tensor_reg.storage);
  auto storage = Downcast<StorageValue>(storage_obj);
  std::shared_ptr<memory_pool::Memory> mem = nullptr;
  if (instr.alloc_tensor_reg.own) {
    mem = storage->buffer;
  }
  auto tensor = TensorValue::Assemble(storage->buffer->device, instr.alloc_tensor_reg.dtype, shape,
                                      {}, storage->buffer->data, mem);
  ctx.WriteRegister(instr.dst, tensor);
  ctx->pc++;
}

void VirtualMachine::HandleAllocTuple(VMContext& ctx, const Instruction& instr) {
  Array<Value> fields;
  for (Index i = 0; i < instr.alloc_tuple.num_fields; ++i) {
    fields.push_back(ctx.ReadRegister(instr.alloc_tuple.fields[i]));
  }
  ctx.WriteRegister(instr.dst, TupleValue::make(fields));
  ctx->pc++;
}

void VirtualMachine::HandleAllocClosure(VMContext& ctx, const Instruction& instr) {
  std::vector<Value> free_vars;
  for (Index i = 0; i < instr.alloc_closure.num_free_vars; i++) {
    free_vars.push_back(ctx.ReadRegister(instr.alloc_closure.free_vars[i]));
  }
  auto clo = VMClosureValue::make(instr.alloc_closure.func_index, free_vars);
  ctx.WriteRegister(instr.dst, clo);
  ctx->pc++;
}

void VirtualMachine::HandleFree(VMContext& ctx, const Instruction& instr) {
  RegName reg = instr.free.memory;
  auto reg_val = ctx.ReadRegister(reg);
  if (reg_val->IsInstance<StorageValueObj>()) {
    auto storage_val = Downcast<StorageValue>(reg_val);
    storage_val->buffer.reset();
  } else {
    CHECK(reg_val->IsInstance<TensorValueObj>())
        << "Expected StorageValue or TensorValue, but got " << reg_val->GetTypeKey();
    auto tensor_val = Downcast<TensorValue>(reg_val);
    tensor_val->mem.reset();
  }
  ctx->pc++;
}

void VirtualMachine::HandleInvokeFunc(VMContext& ctx, const Instruction& instr) {
  std::vector<Value> args;
  for (Index i = 0; i < instr.invoke_func.num_args; ++i) {
    args.push_back(ctx.ReadRegister(instr.invoke_func.args[i]));
  }
  ctx.PushFrame(instr.invoke_func.func_index, args, instr.dst);
}

void VirtualMachine::HandleInvokeClosure(VMContext& ctx, const Instruction& instr) {
  auto closure = Downcast<VMClosureValue>(ctx.ReadRegister(instr.invoke_closure.closure));
  std::vector<Value> args;
  for (auto free_var : closure->free_vars) {
    args.push_back(free_var);
  }
  for (Index i = 0; i < instr.invoke_closure.num_args; ++i) {
    args.push_back(ctx.ReadRegister(instr.invoke_closure.args[i]));
  }
  ctx.PushFrame(closure->func_index, args, instr.dst);
}

void VirtualMachine::HandleInvokeJit(VMContext& ctx, const Instruction& instr) {
  OpEnvPtr op_env;
  std::vector<Value> inputs;
  Value output;
  std::string input_str;

  std::tie(op_env, inputs, output, input_str) = PrepareOpEnv(ctx, instr);
#ifdef MNM_USE_CUDA
  if (use_cuda_) {
    WITH_CUDA_PROFILER(devices_[0], op_env->name(), "ComputationOperator", {input_str},
                       { op_env->Execute(inputs, output); });
  } else
#endif
  {  // cpu
    WITH_BASE_PROFILER(devices_[0], op_env->name(), "ComputationOperator", {input_str},
                       { op_env->Execute(inputs, output); });
  }

  // Release workspace memory.
  // TODO(yaoyaoding): It seems that we can not release the workspace once we launched the
  //   kernel. Because the kernel may be in the executing status at this point due to
  //   asynchronous execution. This would cause problem for multi-stream execution.
  std::shared_ptr<Requests> requests = op_env->GetRequests();
  for (size_t i = 0; i < requests->workspace.size(); ++i) {
    Requests::WorkspaceRequest& entry = requests->workspace[i];
    if (entry.nbytes > 0 && entry.memory != nullptr) {
      *entry.dest = nullptr;
      entry.memory.reset();
    }
  }
  ctx->pc++;
}

void VirtualMachine::HandleSetShape(VMContext& ctx, const Instruction& instr) {
  auto data = Downcast<TensorValue>(ctx.ReadRegister(instr.set_shape.data));
  auto raw_shape = ctx.ReadRegister(instr.set_shape.shape);
  std::vector<int64_t> shape;
  if (const auto tuple = raw_shape.as<TupleValueObj>()) {
    for (size_t i = 0; i < tuple->fields.size(); ++i) {
      shape.push_back(Downcast<IntValue>(tuple->fields[i])->value);
    }
  } else {
    raw_shape = CopyTo(raw_shape, Device(DevType::kCPU(), 0));
    shape = common::shape_utils::GetShapeVecFromData(raw_shape);
  }
  ctx.WriteRegister(instr.dst, data.CreateView(shape));
  ctx->pc++;
}

bool VirtualMachine::HandleRet(VMContext& ctx, const Instruction& instr) {
  auto ret_val = ctx.ReadRegister(instr.result);
  auto caller_return_register = ctx.PopFrame();
  if (caller_return_register < 0) {
    // We have hit the point from which we started running, we should return to the caller breaking
    // the dispatch loop.
    ctx->return_register = ret_val;
    return true;
  } else {  // Otherwise we are just returning from a local call.
    ctx.WriteRegister(caller_return_register, ret_val);
    return false;
  }
}

void VirtualMachine::HandleInferType(VMContext& ctx, const Instruction& instr) {
  Array<Value> args;
  for (Index i = 0; i < instr.infer_type.num_args; i++) {
    args.push_back(ctx.ReadRegister(instr.infer_type.args[i]));
  }
  // infer type
  static auto fschema = Op::GetAttrMap<FMNMSchema>("FMNMSchema");
  Value callee = ctx.ReadRegister(instr.invoke_jit.op_reg);
  Type ret_type;
  if (const auto* opv = callee.as<OpValueObj>()) {
    auto call_values = CallValues::make(callee, fschema[opv->op](args));
    auto fty = Downcast<FuncType>(opv->op->checked_type());
    TypeInference ti = Downcast<TypeInference>(fty->type_constraints[0]);
    ret_type = ti->func(call_values);
  } else {
    auto func = callee.as<ClosureValueObj>()->func;
    CHECK_EQ(func->params.size(), args.size());
    auto new_func = Function(func->params, func->body, {}, {});
    for (size_t i = 0; i < args.size(); ++i) {
      new_func->params[i]->checked_type_ = GetType(args[i]);
    }
    new_func = Downcast<Function>(pass::InferType(new_func));
    ctx.WriteRegister(instr.invoke_jit.op_reg, ClosureValue::make({}, new_func));
    FuncType fty = Downcast<FuncType>(new_func->checked_type());
    ret_type = fty->ret_type;
  }
  // get result
  Array<Value> ret_tup;
  auto push_ret_tuple = [&ret_tup](const TensorTypeNode* ty) {
    auto shape = ArrayToIntTuple(ty->shape);
    // compute storage size
    int64_t size = common::shape_utils::BytesCompactTensor(ty);
    ret_tup.push_back(TupleValue::make({shape, ScalarValue::make(size)}));
  };
  if (const auto* ty = ret_type.as<TensorTypeNode>()) {
    push_ret_tuple(ty);
  } else if (const auto* tup = ret_type.as<TupleTypeNode>()) {
    for (size_t i = 0; i < tup->fields.size(); i++) {
      auto ty = tup->fields[i].as<TensorTypeNode>();
      push_ret_tuple(ty);
    }
  } else {
    LOG(FATAL) << "Unknown type " << ret_type->_type_key;
  }
  ctx.WriteRegister(instr.dst, TupleValue::make(ret_tup));
  ctx->pc++;
}

void VirtualMachine::HandleCudaSetStream(VMContext& ctx, const Instruction& instr) {
#ifdef MNM_USE_CUDA
  while (cuda_streams_.size() <= instr.cuda_set_stream.stream_id) {
    cudaStream_t stream;
    CUDA_CALL(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    cuda_streams_.push_back(stream);
  }
  MNMSetStream(Device(DevType::kCUDA(), static_cast<int>(instr.cuda_set_stream.device_id)),
               cuda_streams_[instr.cuda_set_stream.stream_id]);
  ctx->stream_index = instr.cuda_set_stream.stream_id;
  ctx->pc++;
#else
  LOG(FATAL) << "CUDA is not enabled but encounter the instruction: " << instr;
#endif
}

void VirtualMachine::HandleCudaAddEvent(VMContext& ctx, const Instruction& instr) {
#ifdef MNM_USE_CUDA
  CHECK_LT(ctx->stream_index, cuda_streams_.size());
  while (cuda_events_.size() <= instr.cuda_event.event_id) {
    cudaEvent_t event;
    CUDA_CALL(cudaEventCreateWithFlags(&event, cudaEventDisableTiming));
    cuda_events_.push_back(event);
  }
  CUDA_CALL(
      cudaEventRecord(cuda_events_[instr.cuda_event.event_id], cuda_streams_[ctx->stream_index]));
  ctx->pc++;
#else
  LOG(FATAL) << "CUDA is not enabled but encounter the instruction: " << instr;
#endif
}

void VirtualMachine::HandleCudaWaitEvent(VMContext& ctx, const Instruction& instr) {
#ifdef MNM_USE_CUDA
  CHECK_LT(ctx->stream_index, cuda_streams_.size());
  CHECK_LT(instr.cuda_event.event_id, cuda_events_.size()) << "Wait for non-existing event";
  CUDA_CALL(cudaStreamWaitEvent(cuda_streams_[ctx->stream_index],
                                cuda_events_[instr.cuda_event.event_id],
                                0 /*cudaEventWaitDefault*/));
  ctx->pc++;
#else
  LOG(FATAL) << "CUDA is not enabled but encounter the instruction: " << instr;
#endif
}

std::tuple<std::shared_ptr<OpEnv>, std::vector<Value>, Value, std::string>
VirtualMachine::PrepareOpEnv(const VMContext& ctx, const Instruction& instr) {
  Index num_inputs = instr.invoke_jit.arity - instr.invoke_jit.output_size;
  Array<Value> args;
  Value output;

  auto ftensor_str = [](std::ostringstream& os, const TensorValueObj* tensor) {
    const DLTensor* t = tensor->tensor.operator->();
    os << "T<";
    for (int i = 0; i < t->ndim; ++i) {
      os << t->shape[i] << "x";
    }
    switch (t->dtype.code) {
      case kDLInt: {
        os << "i" << static_cast<int>(t->dtype.bits);
        break;
      }
      case kDLUInt: {
        os << "u" << static_cast<int>(t->dtype.bits);
        break;
      }
      case kDLFloat: {
        os << "f" << static_cast<int>(t->dtype.bits);
        break;
      }
      case kDLBfloat: {
        os << "bf" << static_cast<int>(t->dtype.bits);
        break;
      }
      default: {
        os << "unk";
        break;
      }
    }
    if (t->dtype.lanes > 1) {
      os << "x" << static_cast<int>(t->dtype.lanes);
    }
    os << ">";
  };

  // extract the input args and prepare the hash key to query op env
  std::ostringstream os;
  for (Index i = 0; i < num_inputs; i++) {
    Index reg_idx = instr.invoke_jit.args[i];
    auto reg = ctx.ReadRegister(reg_idx);
    args.push_back(reg);
    if (ctx.IsConst(reg_idx)) {
      // Skip constatnts in the hash key
      continue;
    }
    if (auto tensor = reg.as<TensorValueObj>()) {
      ftensor_str(os, tensor);
    } else if (auto tup = reg.as<TupleValueObj>()) {
      os << "(";
      for (auto field : tup->fields) {
        auto t = field.as<TensorValueObj>();
        CHECK(t != nullptr);
        ftensor_str(os, t);
        os << ",";
      }
      os << ")";
    } else {
      LOG(FATAL) << "Unsupported non-const register type: " << reg->GetTypeKey();
    }
    os << ",";
  }
  std::string input_str = os.str();

  // extract the output
  if (instr.invoke_jit.output_size == 1) {
    output = ctx.ReadRegister(instr.invoke_jit.args[num_inputs]);
  } else {
    Array<Value> outs;
    for (Index i = num_inputs; i < instr.invoke_jit.arity; i++) {
      outs.push_back(ctx.ReadRegister(instr.invoke_jit.args[i]));
    }
    output = TupleValue::make(outs);
  }

  // check the OpEnv cache
  std::shared_ptr<OpEnv> op_env;
  auto op_env_cache = op_env_cache_[ctx->func_index]->Get(ctx->pc);
  if (auto p = op_env_cache->Get(input_str)) {
    // Cache hit. Reuse the OpEnv from the cache.
    op_env = *p;
  } else {
    // Create a new OpEnv.
    static auto fschema = Op::GetAttrMap<FMNMSchema>("FMNMSchema");
    auto call_values = CallValues::make();
    Value callee = ctx.ReadRegister(instr.invoke_jit.op_reg);
    const auto* op = callee.as<OpValueObj>();
    const auto* closure = callee.as<ClosureValueObj>();
    call_values->callee = callee;
    if (op) {
      call_values->args = fschema[op->op](args);
    } else {
      call_values->args = MakeListArgs(args);
    }
    call_values->device = devices_[0];
    call_values->out = output;
    op_env = Dispatch(call_values);
    CHECK(op_env != nullptr) << "ValueError: Cannot dispatch "
                             << (op ? op->op->name : PrettyPrint(closure->func)) << " @"
                             << call_values->device.c_str();
    // add to cache
    op_env_cache->Set(input_str, op_env);
  }

  std::shared_ptr<Requests> requests = op_env->GetRequests();
  for (size_t i = 0; i < requests->workspace.size(); i++) {
    Requests::WorkspaceRequest& entry = requests->workspace[i];
    auto buf = memory_pool::Memory::Alloc(entry.device, entry.nbytes);
    entry.memory = buf;
    *entry.dest = buf->data;
  }

  std::vector<Value> inputs;
  for (int i : op_env->arg_indices) {
    CHECK_GE(i, 0) << "Invalid input index: " << i;
    inputs.push_back(args[i]);
  }
  return std::make_tuple(op_env, std::move(inputs), std::move(output), input_str);
}

tvm::runtime::Module CreateVirtualMachine(const Executable* exec, bool enable_cuda_graph) {
  auto vm = make_object<VirtualMachine>(enable_cuda_graph);
  vm->LoadExecutable(exec);
  return tvm::runtime::Module(vm);
}

MNM_REGISTER_GLOBAL("mnm.vm.VirtualMachine").set_body([](tvm::TVMArgs args, tvm::TVMRetValue* rv) {
  tvm::runtime::Module mod = args[0];
  bool enable_cuda_graph = args[1];
  const auto* exec = dynamic_cast<Executable*>(mod.operator->());
  CHECK(exec) << "The virtual machine executable has not been defined yet.";
  *rv = CreateVirtualMachine(exec, enable_cuda_graph);
});

}  // namespace vm
}  // namespace executor
}  // namespace mnm
