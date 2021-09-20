/*!
 * Copyright (c) 2021 by Contributors
 * \file src/op/dialect/cudnn/dropout.cc
 * \brief cuDNN dropout operators.
 */
#include "mnm/ir.h"
#include "mnm/registry.h"
#include "mnm/op_utils.h"
#include "mnm/device.h"
#include "mnm/device_api.h"
#include "../../../common/cuda_utils.h"
#include "../../schema/nn.h"
#include "./cudnn_utils.h"

namespace mnm {
namespace op {
namespace cudnn {

using namespace mnm::ir;
using namespace mnm::memory_pool;
using namespace mnm::value;

int64_t GetDropoutStateSizeInBytes() {
  size_t stateSizeInBytes;
  CUDNN_CALL(cudnnDropoutGetStatesSize(CUDNNThreadEntry::ThreadLocal()->handle, &stateSizeInBytes));
  return stateSizeInBytes;
}

int64_t GetDropoutReserveSpaceSizeInBytes(TensorType x) {
  size_t reserveSpaceSizeInBytes;
  cudnnTensorDescriptor_t xdesc = NormalizeTensorType(x);
  CUDNN_CALL(cudnnDropoutGetReserveSpaceSize(xdesc, &reserveSpaceSizeInBytes));
  CUDNN_CALL(cudnnDestroyTensorDescriptor(xdesc));
  return reserveSpaceSizeInBytes;
}

TensorValue GetDropoutState(double dropout, int64_t seed) {
  Device device(DevType::kCUDA(), 0);
  size_t stateSizeInBytes = GetDropoutStateSizeInBytes();
  cudnnDropoutDescriptor_t dropoutDesc;
  CUDNN_CALL(cudnnCreateDropoutDescriptor(&dropoutDesc));
  std::shared_ptr<Memory> memory = memory_pool::Memory::Alloc(device, stateSizeInBytes);
  TensorValue state =
      TensorValue::Assemble(device, DType(DTypeCode::kInt(), 8),
                            {static_cast<int64_t>(stateSizeInBytes)}, {}, memory->data, memory);
  DLTensor* dlt = state;
  CUDNN_CALL(cudnnSetDropoutDescriptor(dropoutDesc, CUDNNThreadEntry::ThreadLocal()->handle,
                                       dropout, dlt->data, stateSizeInBytes,
                                       static_cast<uint64_t>(seed)));
  CUDNN_CALL(cudnnDestroyDropoutDescriptor(dropoutDesc));
  return state;
}

MNM_REGISTER_GLOBAL("mnm.backend.cudnn.GetDropoutStateSizeInBytes")
    .set_body_typed(GetDropoutStateSizeInBytes);
MNM_REGISTER_GLOBAL("mnm.backend.cudnn.GetDropoutState").set_body_typed(GetDropoutState);
MNM_REGISTER_GLOBAL("mnm.backend.cudnn.GetDropoutReserveSpaceSizeInBytes")
    .set_body_typed(GetDropoutReserveSpaceSizeInBytes);

static auto fschema_index = ir::Op::GetAttrMap<op::FMNMSchemaFieldIndex>("FMNMSchemaFieldIndex");

class DropoutImplementedByCUDNNDropoutForward : public mnm::op::OpEnv {
  cudnnDropoutDescriptor_t dropoutDesc;
  cudnnTensorDescriptor_t xdesc;
  cudnnTensorDescriptor_t ydesc;
  float dropout;
  size_t stateSizeInBytes;
  size_t reserveSpaceSizeInBytes;

  explicit DropoutImplementedByCUDNNDropoutForward(const CallValues& cv) {
    auto op = Op::Get("mnm.op._contrib_dropout");
    this->arg_indices = {
        fschema_index[op]("x"),
        fschema_index[op]("in_states"),
    };
    auto args = cv->args.as<mnm::op::schema::DropoutArgs>();
    TupleValue tv = Downcast<TupleValue>(cv->out);
    DLTensor* x = args->x;
    DLTensor* out = tv->fields[0];
    DLTensor* state = args->in_states.value();
    xdesc = NormalizeTensorType(SquashTensorShape(x, {}));
    ydesc = NormalizeTensorType(SquashTensorShape(out, {}));
    dropout = args->p;
    CUDNN_CALL(cudnnCreateDropoutDescriptor(&dropoutDesc));
    CUDNN_CALL(
        cudnnDropoutGetStatesSize(CUDNNThreadEntry::ThreadLocal()->handle, &stateSizeInBytes));
    CUDNN_CALL(cudnnDropoutGetReserveSpaceSize(xdesc, &reserveSpaceSizeInBytes));
    CUDNN_CALL(cudnnRestoreDropoutDescriptor(dropoutDesc, CUDNNThreadEntry::ThreadLocal()->handle,
                                             dropout, state->data, stateSizeInBytes, 0));
  }

 public:
  ~DropoutImplementedByCUDNNDropoutForward() {
    CUDNN_CALL(cudnnDestroyTensorDescriptor(xdesc));
    CUDNN_CALL(cudnnDestroyTensorDescriptor(ydesc));
    CUDNN_CALL(cudnnDestroyDropoutDescriptor(dropoutDesc));
  }

  std::string name() const override {
    return TruncateName(GetUniqueName("mnm.op.cudnn._contrib_dropout"));
  }

  void Execute(const CallValues& cv) {
    auto args = cv->args.as<mnm::op::schema::DropoutArgs>();
    CHECK(args != nullptr);
    TupleValue tv = Downcast<TupleValue>(cv->out);
    DLTensor* x = args->x;
    DLTensor* out = tv->fields[0];
    DLTensor* reserve_space = tv->fields[3];

    CUDNN_CALL(cudnnDropoutForward(CUDNNThreadEntry::ThreadLocal()->handle, dropoutDesc, xdesc,
                                   x->data, ydesc, out->data, reserve_space->data,
                                   reserveSpaceSizeInBytes));
  }

  void Execute(const std::vector<Value>& inputs, Value output) {
    CHECK_EQ(inputs.size(), 2);
    TupleValue tv = Downcast<TupleValue>(output);
    DLTensor* x = inputs[0];
    DLTensor* out = tv->fields[0];
    DLTensor* reserve_space = tv->fields[3];

    CUDNN_CALL(cudnnDropoutForward(CUDNNThreadEntry::ThreadLocal()->handle, dropoutDesc, xdesc,
                                   x->data, ydesc, out->data, reserve_space->data,
                                   reserveSpaceSizeInBytes));
  }

  static OpEnv* make(const CallValues& cv) {
    return new DropoutImplementedByCUDNNDropoutForward(cv);
  }
};

MNM_REGISTER_DIALECT_OP(cudnn, _contrib_dropout, 15);
MNM_OP_ENV_MAKER("mnm.op.cudnn._contrib_dropout", DropoutImplementedByCUDNNDropoutForward::make);

class DropoutImplementedByCUDNNDropoutBackward : public mnm::op::OpEnv {
  cudnnDropoutDescriptor_t dropoutDesc;
  cudnnTensorDescriptor_t dxdesc;
  cudnnTensorDescriptor_t dydesc;
  float dropout;
  size_t stateSizeInBytes;
  size_t reserveSpaceSizeInBytes;
  std::shared_ptr<Memory> states;

  explicit DropoutImplementedByCUDNNDropoutBackward(const CallValues& cv) {
    this->arg_indices = {/*dy=*/0, /*reserve_space=*/1};
    auto args = cv->args.as<mnm::op::schema::DropoutDxArgs>();
    DLTensor* dx = cv->out;
    DLTensor* dy = args->dy;
    DLTensor* reserve_space = args->reserve_space;
    dxdesc = NormalizeTensorType(SquashTensorShape(dx, {}));
    dydesc = NormalizeTensorType(SquashTensorShape(dy, {}));
    dropout = args->p;
    reserveSpaceSizeInBytes = ComputeStorageInBytes(SquashTensorShape(reserve_space, {}));
    CUDNN_CALL(cudnnCreateDropoutDescriptor(&dropoutDesc));
    CUDNN_CALL(
        cudnnDropoutGetStatesSize(CUDNNThreadEntry::ThreadLocal()->handle, &stateSizeInBytes));
    states = Memory::Alloc(cv->device, stateSizeInBytes);
    CUDNN_CALL(cudnnSetDropoutDescriptor(dropoutDesc, CUDNNThreadEntry::ThreadLocal()->handle,
                                         dropout, states->data, stateSizeInBytes, 0));
  }

 public:
  ~DropoutImplementedByCUDNNDropoutBackward() {
    CUDNN_CALL(cudnnDestroyTensorDescriptor(dxdesc));
    CUDNN_CALL(cudnnDestroyTensorDescriptor(dydesc));
    CUDNN_CALL(cudnnDestroyDropoutDescriptor(dropoutDesc));
  }

  std::string name() const override {
    return TruncateName(GetUniqueName("mnm.op.cudnn._contrib_dropout_dx"));
  }

  void Execute(const CallValues& cv) {
    auto args = cv->args.as<mnm::op::schema::DropoutDxArgs>();
    CHECK(args != nullptr);
    DLTensor* dx = cv->out;
    DLTensor* dy = args->dy;
    DLTensor* reserve_space = args->reserve_space;

    CUDNN_CALL(cudnnDropoutBackward(CUDNNThreadEntry::ThreadLocal()->handle, dropoutDesc, dydesc,
                                    dy->data, dxdesc, dx->data, reserve_space->data,
                                    reserveSpaceSizeInBytes));
  }

  void Execute(const std::vector<Value>& inputs, Value output) {
    CHECK_EQ(inputs.size(), 2);
    DLTensor* dx = output;
    DLTensor* dy = inputs[0];
    DLTensor* reserve_space = inputs[1];

    CUDNN_CALL(cudnnDropoutBackward(CUDNNThreadEntry::ThreadLocal()->handle, dropoutDesc, dydesc,
                                    dy->data, dxdesc, dx->data, reserve_space->data,
                                    reserveSpaceSizeInBytes));
  }

  static OpEnv* make(const CallValues& cv) {
    return new DropoutImplementedByCUDNNDropoutBackward(cv);
  }
};

MNM_REGISTER_DIALECT_OP(cudnn, _contrib_dropout_dx, 15);
MNM_OP_ENV_MAKER("mnm.op.cudnn._contrib_dropout_dx",
                 DropoutImplementedByCUDNNDropoutBackward::make);

}  // namespace cudnn
}  // namespace op
}  // namespace mnm
