// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef XLS_DSLX_INTERPRETER_H_
#define XLS_DSLX_INTERPRETER_H_

#include "xls/dslx/cpp_ast.h"
#include "xls/dslx/import_routines.h"
#include "xls/dslx/interp_bindings.h"
#include "xls/dslx/interp_callback_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_info.h"
#include "xls/ir/package.h"

namespace xls::dslx {

class Interpreter {
 public:
  // Helper used by type inference to evaluate derived parametric expressions --
  // creates an interpreter and evaluates "expr".
  //
  // Args:
  //  entry_module: Entry-point module to be used in creating the interpreter.
  //  type_info: Type information (derived for the entry point) to be used in
  //    creating the interpreter.
  //  typecheck/import_cache: Supplemental helpers used for import statements.
  //  env: Envionment of current parametric bindings.
  //  bit_widths: Bit widths for parametric bindings.
  //  expr: (Derived parametric) expression to evaluate.
  //  fn_ctx: Current function context.
  //
  // TODO(leary): 2020-11-24 This signature is for backwards compatibility with
  // a Python API, we can likely eliminate it when everything is ported over to
  // C++, or at least consolidate the env/bit_widths maps.
  static absl::StatusOr<int64> InterpretExpr(
      Module* entry_module, const std::shared_ptr<TypeInfo>& type_info,
      TypecheckFn typecheck,
      absl::Span<std::string const> additional_search_path,
      ImportCache* import_cache,
      const absl::flat_hash_map<std::string, int64>& env,
      const absl::flat_hash_map<std::string, int64>& bit_widths, Expr* expr,
      const FnCtx& fn_ctx);

  // Creates an interpreter that can be used to interpreting entities
  // (functions, tests) within the given module.
  //
  // Note: typecheck and import_cache args will likely be-provided or
  // not-be-provided together because they are both used in service of import
  // facilities.
  //
  // Args:
  //  module: "Entry" module wherein functions / tests are being interpreted by
  //    this interpreter.
  //  type_info: Type information associated with the given module -- evaluation
  //    of some AST nodes relies on this type information.
  //  typecheck: Optional, callback used to check modules on import.
  //  additional_search_paths: Additional paths to search for imported modules.
  //  import_cache: Optional, cache for imported modules.
  //  trace_all: Whether to trace "all" (really most "non-noisy") expressions in
  //    the interpreter evaluation.
  //  ir_package: IR-converted form of the given module, used for JIT execution
  //    engine comparison purposes when provided.
  Interpreter(Module* module, const std::shared_ptr<TypeInfo>& type_info,
              TypecheckFn typecheck,
              absl::Span<std::string const> additional_search_paths,
              ImportCache* import_cache, bool trace_all = false,
              Package* ir_package = nullptr);

  // Since we capture pointers to "this" in lambdas, we don't want this object
  // to move/copy/assign.
  Interpreter(Interpreter&&) = delete;
  Interpreter(const Interpreter&) = delete;
  Interpreter& operator=(const Interpreter&) = delete;

  // Runs a function with the given "name" from the module associated with the
  // interpreter, using the given "args" for the entry point invocation.
  // If this function is parametric, then symbolic_bindings needs to contain an
  // entry for each function parameter.
  absl::StatusOr<InterpValue> RunFunction(
      absl::string_view name, absl::Span<const InterpValue> args,
      SymbolicBindings symbolic_bindings = SymbolicBindings());

  // Searches for a test function with the given name in this interpreter's
  // module and, if found, runs it.
  absl::Status RunTest(absl::string_view name);

  absl::StatusOr<InterpValue> EvaluateLiteral(Expr* expr);

  Module* module() const { return module_; }
  Package* ir_package() const { return ir_package_; }

 private:
  friend struct TypeInfoSwap;
  friend class Evaluator;

  struct TypeInfoSwap {
    TypeInfoSwap(Interpreter* parent,
                 const absl::optional<std::shared_ptr<TypeInfo>>& new_type_info)
        : parent_(parent), old_type_info_(parent->type_info_) {
      if (new_type_info.has_value()) {
        parent->type_info_ = new_type_info.value();
      }
    }

    ~TypeInfoSwap() { parent_->type_info_ = old_type_info_; }

    Interpreter* parent_;
    std::shared_ptr<TypeInfo> old_type_info_;
  };

  // Entry point for evaluating an expression to a value.
  //
  // Args:
  //   expr: Expression AST node to evaluate.
  //   bindings: Current bindings for this evaluation (i.e. ident: value map).
  //   type_context: If a type is deduced from surrounding context, it is
  //     provided via this argument.
  //
  // Returns:
  //   The value that the AST node evaluates to.
  //
  // Raises:
  //   EvaluateError: If an error occurs during evaluation. This also attempts
  //   to
  //     print a rough expression-stack-trace for determining the provenance of
  //     an error to ERROR log.
  absl::StatusOr<InterpValue> Evaluate(Expr* expr, InterpBindings* bindings,
                                       ConcreteType* type_context);

  // Evaluates an Invocation AST node to a value.
  absl::StatusOr<InterpValue> EvaluateInvocation(Invocation* expr,
                                                 InterpBindings* bindings,
                                                 ConcreteType* type_context);

  // Wraps function evaluation to compare with JIT execution.
  //
  // If this interpreter was not created with an IR package, this simply
  // evaluates the function. Otherwise, the function is executed with the LLVM
  // JIT and its return value is compared against the interpreted value as a
  // consistency check.
  //
  // TODO(leary): 2020-11-19 This is ok to run twice because there are no side
  // effects -- need to consider what happens when there are side effects (e.g.
  // fatal errors).
  //
  // Args:
  //  f: Function to evaluate
  //  args: Arguments used to invoke f
  //  span: Span of the invocation causing this evaluation
  //  symbolic_bindings: Used if the function is parametric
  //
  // Returns the value that results from interpretation.
  absl::StatusOr<InterpValue> EvaluateAndCompare(
      Function* f, absl::Span<const InterpValue> args, const Span& span,
      Invocation* expr, const SymbolicBindings* symbolic_bindings);

  // Calls function values, either a builtin or user defined function.
  absl::StatusOr<InterpValue> CallFnValue(
      const InterpValue& fv, absl::Span<InterpValue const> args,
      const Span& span, Invocation* invocation,
      const SymbolicBindings* symbolic_bindings);

  absl::Status RunJitComparison(Function* f, absl::Span<InterpValue const> args,
                                const SymbolicBindings* symbolic_bindings);

  absl::StatusOr<InterpValue> RunBuiltin(
      Builtin builtin, absl::Span<InterpValue const> args, const Span& span,
      Invocation* invocation, const SymbolicBindings* symbolic_bindings);

  // Helpers used for annotating work-in-progress constants on import, in case
  // of recursive calls in to the interpreter (e.g. when evaluating constant
  // expressions at the top of a module).
  bool IsWip(ConstantDef* c) const;

  // Notes that "c" is in work in progress state indicated by value: nullopt
  // means 'about to evaluate', a value means 'finished evalatuing to this
  // value'. Returns the current state for 'c' (so we can check whether 'c' had
  // a cached result value already).
  //
  // TODO(leary): 2020-11-20 Rework this "note WIP" interface, it's too
  // overloaded in terms of what it's doing, was done this way to minimize the
  // number of callbacks passed across Python/C++ boundary but that should no
  // longer be a concern.
  absl::optional<InterpValue> NoteWip(ConstantDef* c,
                                      absl::optional<InterpValue> value);

  Module* module_;
  std::shared_ptr<TypeInfo> type_info_;
  TypecheckFn typecheck_;
  ImportCache* import_cache_;
  bool trace_all_;
  Package* ir_package_;

  InterpCallbackData callbacks_;

  // Tracking for incomplete module evaluation status; e.g. on recursive calls
  // during module import; see IsWip().
  absl::flat_hash_map<ConstantDef*, absl::optional<InterpValue>> wip_;
};

}  // namespace xls::dslx

#endif  // XLS_DSLX_INTERPRETER_H_
