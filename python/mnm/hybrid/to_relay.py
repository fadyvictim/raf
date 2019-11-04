import ast
import inspect
from typing import Callable, Dict, List, Optional, Set, Tuple

from mnm._lib import relay

from .cfg import CFG, BasicBlock
from .hybrid_utils import NodeVisitor, get_func_name, unbound_constant_expr

SymTab = Dict[str, relay.Var]
FuncTab = Dict[BasicBlock, relay.GlobalVar]
HybridModule = Dict[relay.GlobalVar, relay.Function]


class BB2Relay(NodeVisitor):

    def __init__(self):
        super(BB2Relay, self).__init__(strict=True)

    def _serialize(self, sym_tab: SymTab):
        ret = []

        for arg in self.local_names:
            ret.append(sym_tab[arg])

        return ret

    def run(self, bb: BasicBlock, sym_tab: SymTab, local_names: List[str], func_tab: FuncTab) -> relay.Expr:
        self.local_names = local_names
        self.func_tab = func_tab
        self.jumps = bb.jumps
        let_list = []
        body = None

        for stmt in bb.stmts:
            if isinstance(stmt, ast.Assign):
                let_list.append(self.visit(stmt, sym_tab))
            else:
                assert body is None
                body = self.visit(stmt, sym_tab)

        if body is None:
            body = self.visit(None, sym_tab)

        while let_list:
            body = let_list.pop()(body)
        assert isinstance(body, relay.Expr)

        return body

    def visit_Assign(self, node: ast.Assign, sym_tab: SymTab):
        target, = node.targets
        lhs = relay.Var(name_hint=target.id)
        rhs = node.value(sym_tab)
        sym_tab[target.id] = lhs

        return lambda body: relay.Let(lhs, rhs, body)

    def visit_Return(self, node: ast.Return, sym_tab: SymTab):
        return node.value(sym_tab)

    def visit_NoneType(self, node: None, sym_tab: SymTab):
        jump, = self.jumps
        args = self._serialize(sym_tab)

        return relay.Call(self.func_tab[jump], args)

    def visit_If(self, node: ast.If, sym_tab: SymTab):
        test = node.test(sym_tab)
        args = self._serialize(sym_tab)
        then_, else_ = self.jumps
        then_ = relay.Call(self.func_tab[then_], args)
        else_ = relay.Call(self.func_tab[else_], args)

        return relay.If(test, then_, else_)


def cfg2relay(cfg: CFG, pyfunc: Callable, local_names: List[str], entry: relay.GlobalVar) -> HybridModule:
    # make global vars for functions
    func_name = get_func_name(pyfunc)
    func_tab: FuncTab = {}

    for idx, bb in enumerate(cfg.bbs):
        func_tab[bb] = relay.GlobalVar("{}${}".format(func_name, idx))
    # make function bodies
    hybrid_module: HybridModule = {}

    for bb in cfg.bbs:
        sym_tab = {name: relay.Var(name) for name in local_names}
        body = BB2Relay().run(bb=bb, sym_tab=dict(sym_tab),
                              local_names=local_names, func_tab=func_tab)
        params = [sym_tab[name] for name in local_names]
        func = relay.Function(params=params, body=body)
        global_var = func_tab[bb]
        hybrid_module[global_var] = func

    # make the entry function
    pyargs = list(inspect.signature(pyfunc).parameters.keys())
    named_params = {name: relay.Var(name) for name in pyargs}
    # params: arguments to the wrapper's entry point
    params = [named_params[name] for name in pyargs]
    # args: arguments to feed CFG's entry point
    args = [unbound_constant_expr() if name not in pyargs else named_params[name]
            for name in local_names]
    # realize the entry function
    body = relay.Call(func_tab[cfg.entry], args=args)
    func = relay.Function(params=params, body=body)
    hybrid_module[entry] = func

    return hybrid_module
