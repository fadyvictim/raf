/*!
 * Copyright (c) 2019 by Contributors
 * \file src/op/grad/binary.cc
 * \brief Declaration of gradients
 */
#include "mnm/op.h"

namespace mnm {
namespace op {
namespace grad {

using namespace mnm::ir;

Array<Expr> NllLossGrad(const Var& y, const Expr& orig_call, const Array<Expr>& ograds) {
  static auto dpred = Op::Get("mnm.op.nll_loss_dpred");
  static auto dtrue = Op::Get("mnm.op.nll_loss_dtrue");
  // TODO(@were): I am not sure how is the dy here.
  // CHECK_EQ(ograds.size(), 1);
  // const Expr& dy = ograds[0];
  const CallNode* call = orig_call.as<CallNode>();
  CHECK_GE(call->args.size(), 2);
  const Expr& pred = call->args[0];
  const Expr& true_ = call->args[1];
  return {CallNode::make(dpred, {pred, true_}), CallNode::make(dtrue, {pred, true_})};
}

MNM_OP_GRAD("mnm.op.nll_loss", NllLossGrad);

}  // namespace grad
}  // namespace op
}  // namespace mnm