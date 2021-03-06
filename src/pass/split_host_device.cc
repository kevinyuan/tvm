/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file split_host_device.cc
 * \brief Split device function from host.
 */
#include <tvm/ir.h>
#include <tvm/lowered_func.h>
#include <tvm/ir_pass.h>
#include <tvm/ir_functor_ext.h>
#include <tvm/runtime/module.h>
#include <unordered_map>

namespace tvm {
namespace ir {

// use/def analysis, also delete unreferenced lets
class IRUseDefAnalysis : public StmtExprMutator {
 public:
  Stmt VisitStmt_(const AttrStmt* op) final {
    if (op->attr_key == attr::thread_extent) {
      IterVar iv = Downcast<IterVar>(op->node);
      CHECK_NE(iv->thread_tag.length(), 0U);
      // thread_extent can appear multiple times
      // use the first appearance as def.
      if (!use_count_.count(iv->var.get())) {
        this->HandleDef(iv->var.get());
        thread_axis_.push_back(iv);
        thread_extent_.push_back(op->value);
      }

      Expr value = op->value;
      if (visit_thread_extent_) {
        value = this->VisitExpr(value);
      }
      Stmt body = this->VisitStmt(op->body);
      if (value.same_as(op->value) && body.same_as(op->body)) {
        return GetRef<Stmt>(op);
      }
      return AttrStmt::make(op->node, op->attr_key, value, body);
    } else {
      return StmtExprMutator::VisitStmt_(op);
    }
  }

  Stmt VisitStmt_(const LetStmt* op) final {
    this->HandleDef(op->var.get());
    Stmt body = this->VisitStmt(op->body);
    // eliminate unreferenced let
    if (use_count_.at(op->var.get()) == 0 &&
        !HasSideEffect(op->value)) {
      return body;
    } else {
      Expr value = this->VisitExpr(op->value);
      if (body.same_as(op->body) &&
          value.same_as(op->value)) {
        return GetRef<Stmt>(op);
      } else {
        return LetStmt::make(op->var, value, body);
      }
    }
  }

  Stmt VisitStmt_(const For* op) final {
    this->HandleDef(op->loop_var.get());
    return StmtExprMutator::VisitStmt_(op);
  }

  Stmt VisitStmt_(const Allocate* op) final {
    this->HandleDef(op->buffer_var.get());
    return StmtExprMutator::VisitStmt_(op);
  }

  Stmt VisitStmt_(const Store* op) final {
    this->HandleUse(op->buffer_var);
    return StmtExprMutator::VisitStmt_(op);
  }

  Expr VisitExpr_(const Let* op) final {
    this->HandleDef(op->var.get());
    Expr body = this->VisitExpr(op->body);
    // eliminate unreferenced let
    if (use_count_.at(op->var.get()) == 0 &&
        !HasSideEffect(op->value)) {
      return body;
    } else {
      Expr value = this->VisitExpr(op->value);
      if (body.same_as(op->body) &&
          value.same_as(op->value)) {
        return GetRef<Expr>(op);
      } else {
        return Let::make(op->var, value, body);
      }
    }
  }

  Expr VisitExpr_(const Variable* op) final {
    this->HandleUse(GetRef<Expr>(op));
    return StmtExprMutator::VisitExpr_(op);
  }

  Expr VisitExpr_(const Load* op) final {
    this->HandleUse(op->buffer_var);
    return StmtExprMutator::VisitExpr_(op);
  }

  void HandleDef(const Variable* v) {
    CHECK(!def_count_.count(v))
        << "variable " << v->name_hint
        << " has already been defined, the Stmt is not SSA";
    CHECK(!use_count_.count(v))
        << "variable " << v->name_hint
        << " has been used before definition!";
    use_count_[v] = 0;
    def_count_[v] = 1;
  }

  void HandleUse(const Expr& v) {
    CHECK(v.as<Variable>());
    Var var = Downcast<Var>(v);
    auto it = use_count_.find(var.get());
    if (it != use_count_.end()) {
      if (it->second >= 0) {
        ++it->second;
      }
    } else {
      undefined_.push_back(var);
      use_count_[var.get()] = -1;
    }
  }

  // The fields are publically readible to
  // be accessible to the users.
  bool visit_thread_extent_{true};
  Array<Var> undefined_;
  Array<IterVar> thread_axis_;
  Array<Expr> thread_extent_;
  std::unordered_map<const Variable*, int> use_count_;
  std::unordered_map<const Variable*, int> def_count_;
};

class HostDeviceSplitter : public StmtMutator {
 public:
  Stmt VisitStmt_(const Allocate* op) final {
    handle_data_type_[op->buffer_var.get()] = make_const(op->dtype, 0);
    return StmtMutator::VisitStmt_(op);
  }

  Stmt VisitStmt_(const AttrStmt* op) final {
    if (op->attr_key == attr::thread_extent ||
        op->attr_key == attr::pipeline_exec_scope ||
        op->attr_key == attr::device_scope) {
      return SplitDeviceFunc(GetRef<Stmt>(op));
    }
    return StmtMutator::VisitStmt_(op);
  }

  Array<LoweredFunc> Split(LoweredFunc f) {
    CHECK_EQ(f->func_type, kMixedFunc);
    for (auto kv : f->handle_data_type) {
      handle_data_type_[kv.first.get()] = kv.second;
    }
    name_ = f->name;
    ObjectPtr<LoweredFuncNode> n =
        make_object<LoweredFuncNode>(*f.operator->());
    n->body = operator()(f->body);
    n->func_type = kHostFunc;
    Array<LoweredFunc> ret{LoweredFunc(n)};
    for (LoweredFunc x : device_funcs_) {
      ret.push_back(x);
    }
    return ret;
  }

 private:
  Stmt SplitDeviceFunc(Stmt body) {
    std::ostringstream os;
    os << name_ << "_kernel" << device_funcs_.size();
    ObjectPtr<LoweredFuncNode> n = make_object<LoweredFuncNode>();
    // isolate the device function.
    IRUseDefAnalysis m;
    m.visit_thread_extent_ = false;
    n->body = m(std::move(body));
    n->name = os.str();
    n->func_type = kDeviceFunc;
    n->thread_axis = m.thread_axis_;
    // Strictly order the arguments: Var pointers, positional arguments.
    for (Var v : m.undefined_) {
      if (v.dtype().is_handle()) {
        n->args.push_back(v);
        // mark handle data type.
        auto it = handle_data_type_.find(v.get());
        if (it != handle_data_type_.end()) {
          n->handle_data_type.Set(v, it->second);
        }
      }
    }
    for (Var v : m.undefined_) {
      if (!v.dtype().is_handle()) {
        n->args.push_back(v);
      }
    }
    LoweredFunc f_device(n);
    Array<Expr> call_args;
    call_args.push_back(StringImm::make(f_device->name));
    for (Var arg : n->args) {
      call_args.push_back(arg);
    }
    for (Expr ext : m.thread_extent_) {
      call_args.push_back(ext);
    }
    device_funcs_.emplace_back(f_device);
    return Evaluate::make(Call::make(
        DataType::Int(32), intrinsic::tvm_call_packed,
        call_args, Call::Intrinsic));
  }

  // function name
  std::string name_;
  // the device functions
  std::vector<LoweredFunc> device_funcs_;
  std::unordered_map<const Variable*, Expr> handle_data_type_;
};


Array<Var> UndefinedVars(const Stmt& stmt, const Array<Var>& args) {
  IRUseDefAnalysis m;
  for (Var arg : args) {
    m.use_count_[arg.get()] = 0;
  }
  m(stmt);
  return m.undefined_;
}

Array<LoweredFunc> SplitHostDevice(LoweredFunc func) {
  return HostDeviceSplitter().Split(func);
}

}  // namespace ir
}  // namespace tvm
