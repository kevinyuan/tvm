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
 * \file inline.cc
 */
#include <tvm/ir.h>
#include <tvm/ir_pass.h>
#include <tvm/ir_functor_ext.h>

namespace tvm {
namespace ir {

// inliner to inline a function
// the result may not be SSA,
// ConvertSSA need to be applied after this pass
class IRInline final : public StmtExprMutator {
 public:
  IRInline(FunctionRef f, Array<Var> args, Expr body)
      : f_(f), args_(args), body_(body) {}

  Expr VisitExpr_(const Call* op) final {
    Expr expr = StmtExprMutator::VisitExpr_(op);
    op = expr.as<Call>();

    if (op->func == f_) {
      CHECK_EQ(op->value_index, 0);
      expr = body_;
      CHECK_EQ(args_.size(), op->args.size());

      bool has_side_effect = false;
      for (size_t i = 0; i < op->args.size(); ++i) {
        if (HasSideEffect(op->args[i])) has_side_effect = true;
      }
      if (has_side_effect) {
        for (size_t i = 0; i < args_.size(); ++i) {
          expr = Let::make(args_[i], op->args[i], expr);
        }
      } else {
        Map<Var, Expr> vmap;
        for (size_t i = 0; i < args_.size(); ++i) {
          vmap.Set(args_[i], op->args[i]);
        }
        expr = Substitute(
            Evaluate::make(expr), vmap).as<Evaluate>()->value;
      }
      return expr;
    } else {
      return expr;
    }
  }

 private:
  FunctionRef f_;
  Array<Var> args_;
  Expr body_;
};

Stmt Inline(Stmt stmt,
            FunctionRef f,
            Array<Var> args,
            Expr body) {
  CHECK_EQ(f->num_outputs(), 1)
      << "can only inline output single value operation";
  Stmt ret = IRInline(f, args, body)(std::move(stmt));
  if (ret.same_as(stmt)) return ret;
  return ConvertSSA(ret);
}
}  // namespace ir
}  // namespace tvm
