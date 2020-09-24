# pylint: disable=missing-function-docstring
"""Compute definition and schedules for loss functions."""
from .._lib import register_compute
from .._lib import tvm as _tvm
from .._lib import _reg

_topi = _tvm.topi  # pylint: disable=invalid-name,no-member

@register_compute("mnm.op.nll_loss")
def nll_loss_compute(attrs, inputs, output_type):  # pylint: disable=unused-argument
    true, pred = inputs
    n, c = pred.shape
    redn = _tvm.te.reduce_axis((0, n), name='rn')
    redc = _tvm.te.reduce_axis((0, c), name='rc')

    def fcompute(x):  # pylint: disable=unused-argument
        return _tvm.te.sum(-pred[redn, redc] * true[redn, redc] / n,
                           axis=[redc, redn])

    loss = _tvm.te.compute((1, ), fcompute)
    return [loss]


_reg.register_injective_schedule("mnm.op.nll_loss")


@register_compute("mnm.op.nll_loss_dpred")
def nllloss_dpred_compute(attr, inputs, output_type):  # pylint: disable=unused-argument
    true, _ = inputs
    n, c = true.shape
    return [_tvm.te.compute((n, c), lambda x, y: -true[x, y] / n)]


_reg.register_broadcast_schedule("mnm.op.nll_loss_dpred")


@register_compute("mnm.op.nll_loss_dtrue")
def nllloss_dtrue_compute(attr, inputs, output_type):  # pylint: disable=unused-argument
    _, pred = inputs
    n, c = pred.shape
    return [_tvm.te.compute((n, c), lambda x, y: -pred[x, y] / n)]


_reg.register_broadcast_schedule("mnm.op.nll_loss_dtrue")


@register_compute("mnm.op.cross_entropy")
def cross_entropy_compute(attr, inputs, output_type):  # pylint: disable=unused-argument
    true, pred = inputs
    n, c = pred.shape
    redn = _tvm.te.reduce_axis((0, n), name='rn')
    redc = _tvm.te.reduce_axis((0, c), name='rc')

    pred_log_sm = _topi.nn.log_softmax(pred)

    def fcompute(x):  # pylint: disable=unused-argument
        return _tvm.te.sum(-pred_log_sm[redn, redc] * true[redn, redc] / n,
                           axis=[redc, redn])

    loss = _tvm.te.compute((1, ), fcompute)
    return [loss]


_reg.register_broadcast_schedule("mnm.op.cross_entropy")

@register_compute("mnm.op.cross_entropy_dpred")
def cross_entropy_dpred_compute(attr, inputs, output_type):  # pylint: disable=unused-argument
    true, pred = inputs
    n, c = true.shape
    pred_sm = _topi.nn.softmax(pred)

    return [_tvm.te.compute((n, c), lambda x, y: (-true[x, y] + pred_sm[x, y]) / n)]


_reg.register_broadcast_schedule("mnm.op.cross_entropy_dpred")


@register_compute("mnm.op.cross_entropy_dtrue")
def cross_entropy_dtrue_compute(attr, inputs, output_type):  # pylint: disable=unused-argument
    _, pred = inputs
    n, c = pred.shape
    pred_log_sm = _topi.nn.log_softmax(pred)
    return [_tvm.te.compute((n, c), lambda x, y: -pred_log_sm[x, y] / n)]


_reg.register_broadcast_schedule("mnm.op.cross_entropy_dtrue")
