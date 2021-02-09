import pytest
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

import mnm
from mnm.model import Conv2d, Linear, BatchNorm
from mnm.testing import with_seed, get_device_list, check, run_vm_model, \
    one_hot_torch, randn_torch, t2m_param


class TorchTest(nn.Module):  # pylint: disable=abstract-method
    def __init__(self, input_shape=28, num_classes=10):
        super(TorchTest, self).__init__()
        self.conv1 = nn.Conv2d(in_channels=3,
                               out_channels=6,
                               kernel_size=5,
                               padding=2,
                               bias=False)
        self.bn1 = nn.BatchNorm2d(6)
        self.linear1 = nn.Linear((input_shape // 2) ** 2 * 6, num_classes)

    def forward(self, x, y_true): # pylint: disable=arguments-differ
        y_pred = self.forward_infer(x)
        y_pred = F.log_softmax(y_pred, dim=-1)
        loss = F.nll_loss(y_pred, y_true)
        return loss

    def forward_infer(self, x):
        out = self.bn1(self.conv1(x))
        out = torch.sigmoid(out) # pylint: disable=no-member
        out = F.avg_pool2d(out, (2, 2), (2, 2))
        out = torch.flatten(out, 1) # pylint: disable=no-member
        out = self.linear1(out)
        return out


class MNMTest(mnm.Model):
    # pylint: disable=attribute-defined-outside-init
    def build(self, input_shape=28, num_classes=10):
        self.conv1 = Conv2d(in_channels=3,
                            out_channels=6,
                            kernel_size=5,
                            padding=2,
                            bias=False)
        self.bn1 = BatchNorm(6)
        self.linear1 = Linear((input_shape // 2) ** 2 * 6,
                              num_classes)
    # pylint: enable=attribute-defined-outside-init

    @mnm.model.trace
    def forward(self, x, y_true):
        y_pred = self.forward_infer(x)
        y_pred = mnm.log_softmax(y_pred)
        loss = mnm.nll_loss(y_true=y_true, y_pred=y_pred)
        return loss

    @mnm.model.trace
    def forward_infer(self, x):
        out = self.bn1(self.conv1(x))
        out = mnm.sigmoid(out)
        out = mnm.avg_pool2d(out, (2, 2), (2, 2))
        out = mnm.batch_flatten(out)
        out = self.linear1(out)
        return out


# pylint: disable=unused-variable
@with_seed(0)
@pytest.mark.skipif(not mnm.build.with_cuda(), reason="CUDA is not enabled")
@pytest.mark.parametrize("config", [
    (10, 32, 10),
    (4, 28, 10),
])
def test_sgd(config):
    t_model = TorchTest(config[1], config[2])
    t_model.to(device='cuda')
    m_model = MNMTest(config[1], config[2])
    m_model.to(device='cuda')
    m_model.conv1.w = t2m_param(t_model.conv1.weight)
    m_model.linear1.w = t2m_param(t_model.linear1.weight)
    m_model.linear1.b = t2m_param(t_model.linear1.bias)
    m_model.bn1.w = t2m_param(t_model.bn1.weight)
    m_model.bn1.b = t2m_param(t_model.bn1.bias)
    m_model.bn1.running_mean = t2m_param(t_model.bn1.running_mean)
    m_model.bn1.running_var = t2m_param(t_model.bn1.running_var)

    m_param_dict = m_model.state()
    m_optimizer = mnm.optim.SGD(m_param_dict.values(), 0.1, 0.01)
    t_optimizer = torch.optim.SGD(t_model.parameters(), lr=0.1, momentum=0.01)
    batch_size = config[0]
    m_model.train_mode()
    t_model.train()

    for i in range(batch_size):
        t_optimizer.zero_grad()
        m_x, t_x = randn_torch([1, 3, config[1], config[1]], requires_grad=True, device="cuda")
        m_x.requires_grad = True
        m_y, t_y = one_hot_torch(batch_size=1, num_classes=config[2], device="cuda")
        m_loss = m_model(m_x, m_y)
        t_loss = t_model(t_x, t_y)
        m_loss.backward()
        t_loss.backward()
        check(m_loss, t_loss)
        check(m_model.conv1.w.grad, t_model.conv1.weight.grad, rtol=1e-4, atol=1e-4)
        check(m_model.linear1.w.grad, t_model.linear1.weight.grad, rtol=1e-4, atol=1e-4)
        check(m_model.linear1.b.grad, t_model.linear1.bias.grad, rtol=1e-4, atol=1e-4)
        check(m_model.bn1.w.grad, t_model.bn1.weight.grad, rtol=1e-4, atol=1e-4)
        check(m_model.bn1.b.grad, t_model.bn1.bias.grad, rtol=1e-4, atol=1e-4)
        m_optimizer.step()
        t_optimizer.step()
        check(m_model.conv1.w, t_model.conv1.weight, rtol=1e-4, atol=1e-4)
        check(m_model.linear1.w, t_model.linear1.weight, rtol=1e-4, atol=1e-4)
        check(m_model.linear1.b, t_model.linear1.bias, rtol=1e-4, atol=1e-4)
        check(m_model.bn1.w, t_model.bn1.weight, rtol=1e-4, atol=1e-4)
        check(m_model.bn1.b, t_model.bn1.bias, rtol=1e-4, atol=1e-4)


class TorchSimpleTest(nn.Module):  # pylint: disable=abstract-method
    def __init__(self, shape):
        super(TorchSimpleTest, self).__init__()
        self.x = torch.nn.Parameter(torch.randn(*shape))
        self.x.requires_grad = True

    def forward(self): # pylint: disable=arguments-differ
        y = F.relu(self.x)
        return y


class MNMSimpleTest(mnm.Model):
    # pylint: disable=attribute-defined-outside-init
    def build(self, shape):
        self.x = mnm.array(np.random.randn(*shape))

    @mnm.model.trace
    def forward(self):
        y = mnm.relu(self.x)
        return y


@pytest.mark.parametrize("device", get_device_list())
def test_traced_sgd_simple(device):
    # pylint: disable=attribute-defined-outside-init
    shape = (2, 2)
    batch_size = 32
    dtype = 'float32'
    t_model = TorchSimpleTest(shape)
    t_model.to(device)
    m_model = MNMSimpleTest(shape)
    m_model.x = t2m_param(t_model.x, device=device)
    m_model.train_mode()
    t_model.train()
    m_optimizer = mnm.optim.sgd.with_sgd(learning_rate=0.1, momentum=0.01)(m_model)
    t_optimizer = torch.optim.SGD(t_model.parameters(), lr=0.1, momentum=0.01)
    for i in range(batch_size):
        m_dy, t_dy = randn_torch(shape, device=device, requires_grad=False)
        m_loss = run_vm_model(m_optimizer, device, [m_dy])
        t_optimizer.zero_grad()
        t_loss = t_model()
        t_loss.backward(t_dy)
        t_optimizer.step()
        check(m_model.x, t_model.x, rtol=1e-4, atol=1e-4)


@pytest.mark.skipif(not mnm.build.with_cuda(), reason="CUDA is not enabled")
@pytest.mark.parametrize("config", [
    (10, 32, 10),
    (4, 28, 10),
])
def test_traced_sgd(config):
    # pylint: disable=too-many-locals
    device = "cuda"
    t_model = TorchTest(config[1], config[2])
    t_model.to(device=device)
    m_model = MNMTest(config[1], config[2])
    m_model.to(device=device)
    m_model.conv1.w = t2m_param(t_model.conv1.weight, device=device)
    m_model.linear1.w = t2m_param(t_model.linear1.weight, device=device)
    m_model.linear1.b = t2m_param(t_model.linear1.bias, device=device)
    m_model.bn1.w = t2m_param(t_model.bn1.weight, device=device)
    m_model.bn1.b = t2m_param(t_model.bn1.bias, device=device)
    m_model.bn1.running_mean = t2m_param(t_model.bn1.running_mean, device=device)
    m_model.bn1.running_var = t2m_param(t_model.bn1.running_var, device=device)

    batch_size = config[0]
    m_model.train_mode()
    t_model.train()
    m_model.bn1.running_mean.requires_grad = False
    m_model.bn1.running_var.requires_grad = False
    m_optimizer = mnm.optim.sgd.with_sgd(learning_rate=0.1, momentum=0.01)(m_model)
    t_optimizer = torch.optim.SGD(t_model.parameters(), lr=0.1, momentum=0.01)

    for i in range(batch_size):
        m_dy, t_dy = randn_torch((), std=0.0, mean=1.0, device=device, requires_grad=False)
        m_x, t_x = randn_torch([1, 3, config[1], config[1]], requires_grad=True, device=device)
        m_x.requires_grad = True
        m_y, t_y = one_hot_torch(batch_size=1, num_classes=config[2], device=device)
        m_loss = run_vm_model(m_optimizer, device, [m_dy, m_x, m_y])
        t_optimizer.zero_grad()
        t_loss = t_model(t_x, t_y)
        t_loss.backward(t_dy)
        t_optimizer.step()
        check(m_model.conv1.w, t_model.conv1.weight, rtol=1e-4, atol=1e-4)
        check(m_model.linear1.w, t_model.linear1.weight, rtol=1e-4, atol=1e-4)
        check(m_model.linear1.b, t_model.linear1.bias, rtol=1e-4, atol=1e-4)
        check(m_model.bn1.w, t_model.bn1.weight, rtol=1e-4, atol=1e-4)
        check(m_model.bn1.b, t_model.bn1.bias, rtol=1e-4, atol=1e-4)


if __name__ == "__main__":
    pytest.main([__file__])
