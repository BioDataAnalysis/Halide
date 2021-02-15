#include "SlidingWindow.h"

#include "Bounds.h"
#include "CompilerLogger.h"
#include "Debug.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "Monotonic.h"
#include "Scope.h"
#include "Simplify.h"
#include "Solve.h"
#include "Substitute.h"
#include "UnsafePromises.h"
#include <utility>

namespace Halide {
namespace Internal {

using std::map;
using std::string;

namespace {

// Does an expression depend on a particular variable?
class ExprDependsOnVar : public IRVisitor {
    using IRVisitor::visit;

    void visit(const Variable *op) override {
        if (op->name == var) {
            result = true;
        }
    }

    void visit(const Let *op) override {
        op->value.accept(this);
        // The name might be hidden within the body of the let, in
        // which case there's no point descending.
        if (op->name != var) {
            op->body.accept(this);
        }
    }

public:
    bool result;
    string var;

    ExprDependsOnVar(string v)
        : result(false), var(std::move(v)) {
    }
};

bool expr_depends_on_var(const Expr &e, string v) {
    ExprDependsOnVar depends(std::move(v));
    e.accept(&depends);
    return depends.result;
}

class ExpandExpr : public IRMutator {
    using IRMutator::visit;
    const Scope<Expr> &scope;

    Expr visit(const Variable *var) override {
        if (scope.contains(var->name)) {
            Expr expr = scope.get(var->name);
            debug(4) << "Fully expanded " << var->name << " -> " << expr << "\n";
            return expr;
        } else {
            return var;
        }
    }

public:
    ExpandExpr(const Scope<Expr> &s)
        : scope(s) {
    }
};

// Perform all the substitutions in a scope
Expr expand_expr(const Expr &e, const Scope<Expr> &scope) {
    ExpandExpr ee(scope);
    Expr result = ee.mutate(e);
    debug(4) << "Expanded " << e << " into " << result << "\n";
    return result;
}

class FindProduce : public IRVisitor {
    const string &func;

    using IRVisitor::visit;

    void visit(const ProducerConsumer *op) override {
        if (op->is_producer && op->name == func) {
            found = true;
        } else {
            IRVisitor::visit(op);
        }
    }

public:
    bool found = false;

    FindProduce(const string &func) : func(func) {}
};

bool find_produce(const Stmt &s, const string &func) {
    FindProduce finder(func);
    s.accept(&finder);
    return finder.found;
}

// Perform sliding window optimization for a function over a
// particular serial for loop
class SlidingWindowOnFunctionAndLoop : public IRMutator {
    Function func;
    string loop_var;
    Expr loop_min;
    Scope<Expr> scope;

    map<string, Expr> replacements;

    using IRMutator::visit;

    // Check if the dimension at index 'dim_idx' is always pure (i.e. equal to 'dim')
    // in the definition (including in its specializations)
    bool is_dim_always_pure(const Definition &def, const string &dim, int dim_idx) {
        const Variable *var = def.args()[dim_idx].as<Variable>();
        if ((!var) || (var->name != dim)) {
            return false;
        }

        for (const auto &s : def.specializations()) {
            bool pure = is_dim_always_pure(s.definition, dim, dim_idx);
            if (!pure) {
                return false;
            }
        }
        return true;
    }

    Stmt visit(const ProducerConsumer *op) override {
        if (op->is_producer) {
            if (op->name != func.name()) {
                return IRMutator::visit(op);
            }
            Stmt stmt = op;

            // We're interested in the case where exactly one of the
            // dimensions of the buffer has a min/extent that depends
            // on the loop_var.
            string dim = "";
            int dim_idx = 0;
            Expr min_required, max_required;

            debug(3) << "Considering sliding " << func.name()
                     << " along loop variable " << loop_var << "\n"
                     << "Region provided:\n";

            string prefix = func.name() + ".s" + std::to_string(func.updates().size()) + ".";
            const std::vector<string> func_args = func.args();
            for (int i = 0; i < func.dimensions(); i++) {
                // Look up the region required of this function's last stage
                string var = prefix + func_args[i];
                internal_assert(scope.contains(var + ".min") && scope.contains(var + ".max"));
                Expr min_req = scope.get(var + ".min");
                Expr max_req = scope.get(var + ".max");
                min_req = expand_expr(min_req, scope);
                max_req = expand_expr(max_req, scope);

                debug(3) << func_args[i] << ":" << min_req << ", " << max_req << "\n";
                if (expr_depends_on_var(min_req, loop_var) ||
                    expr_depends_on_var(max_req, loop_var)) {
                    if (!dim.empty()) {
                        dim = "";
                        min_required = Expr();
                        max_required = Expr();
                        break;
                    } else {
                        dim = func_args[i];
                        dim_idx = i;
                        min_required = min_req;
                        max_required = max_req;
                    }
                } else if (!min_required.defined() &&
                           i == func.dimensions() - 1 &&
                           is_pure(min_req) &&
                           is_pure(max_req)) {
                    // The footprint doesn't depend on the loop var. Just compute everything on the first loop iteration.
                    dim = func_args[i];
                    dim_idx = i;
                    min_required = min_req;
                    max_required = max_req;
                }
            }

            if (!min_required.defined()) {
                debug(3) << "Could not perform sliding window optimization of "
                         << func.name() << " over " << loop_var << " because multiple "
                         << "dimensions of the function dependended on the loop var\n";
                return stmt;
            }

            // If the function is not pure in the given dimension, give up. We also
            // need to make sure that it is pure in all the specializations
            bool pure = true;
            for (const Definition &def : func.updates()) {
                pure = is_dim_always_pure(def, dim, dim_idx);
                if (!pure) {
                    break;
                }
            }
            if (!pure) {
                debug(3) << "Could not performance sliding window optimization of "
                         << func.name() << " over " << loop_var << " because the function "
                         << "scatters along the related axis.\n";
                return stmt;
            }

            bool can_slide_up = false;
            bool can_slide_down = false;

            Monotonic monotonic_min = is_monotonic(min_required, loop_var);
            Monotonic monotonic_max = is_monotonic(max_required, loop_var);

            if (monotonic_min == Monotonic::Increasing ||
                monotonic_min == Monotonic::Constant) {
                can_slide_up = true;
            } else if (monotonic_min == Monotonic::Unknown) {
                if (get_compiler_logger()) {
                    get_compiler_logger()->record_non_monotonic_loop_var(loop_var, min_required);
                }
            }

            if (monotonic_max == Monotonic::Decreasing ||
                monotonic_max == Monotonic::Constant) {
                can_slide_down = true;
            } else if (monotonic_max == Monotonic::Unknown) {
                if (get_compiler_logger()) {
                    get_compiler_logger()->record_non_monotonic_loop_var(loop_var, max_required);
                }
            }

            if (!can_slide_up && !can_slide_down) {
                debug(3) << "Not sliding " << func.name()
                         << " over dimension " << dim
                         << " along loop variable " << loop_var
                         << " because I couldn't prove it moved monotonically along that dimension\n"
                         << "Min is " << min_required << "\n"
                         << "Max is " << max_required << "\n";
                return stmt;
            }

            // Ok, we've isolated a function, a dimension to slide
            // along, and loop variable to slide over.
            debug(3) << "Sliding " << func.name()
                     << " over dimension " << dim
                     << " along loop variable " << loop_var << "\n";

            Expr loop_var_expr = Variable::make(Int(32), loop_var);

            Expr prev_max_plus_one = substitute(loop_var, loop_var_expr - 1, max_required) + 1;
            Expr prev_min_minus_one = substitute(loop_var, loop_var_expr - 1, min_required) - 1;

            // If there's no overlap between adjacent iterations, we shouldn't slide.
            if (can_prove(min_required >= prev_max_plus_one) ||
                can_prove(max_required <= prev_min_minus_one)) {
                debug(3) << "Not sliding " << func.name()
                         << " over dimension " << dim
                         << " along loop variable " << loop_var
                         << " there's no overlap in the region computed across iterations\n"
                         << "Min is " << min_required << "\n"
                         << "Max is " << max_required << "\n";
                return stmt;
            }

            std::string new_loop_min_name = unique_name('x');
            Expr new_loop_min_var = Variable::make(Int(32), new_loop_min_name);
            Expr new_loop_min_eq;
            if (can_slide_up) {
                new_loop_min_eq =
                    substitute(loop_var, loop_min, min_required) == substitute(loop_var, new_loop_min_var, prev_max_plus_one);
            } else {
                new_loop_min_eq =
                    substitute(loop_var, loop_min, max_required) == substitute(loop_var, new_loop_min_var, prev_min_minus_one);
            }
            // Ignore unsafe promises (intended for the ones generated by
            // TailStrategy::GuardWithIf, but may be relevant in other cases).
            new_loop_min_eq = lower_safe_promises(new_loop_min_eq);
            Interval solve_result = solve_for_inner_interval(new_loop_min_eq, new_loop_min_name);
            Expr new_min, new_max;
            if (solve_result.has_upper_bound() && equal(solve_result.min, solve_result.max)) {
                // There is exactly one solution for where we should start
                // this loop.
                internal_assert(!new_loop_min.defined());
                new_loop_min = simplify(solve_result.max);
                if (equal(new_loop_min, loop_min)) {
                    new_loop_min = Expr();
                }
                if (can_slide_up) {
                    new_min = prev_max_plus_one;
                    new_max = max_required;
                } else {
                    new_min = min_required;
                    new_max = prev_min_minus_one;
                }
            } else {
                // TODO: This is the "old" way of handling sliding window.
                // It handles sliding windows involving upsamples better
                // than the "new" way above. It would be best to fix this,
                // and use the above codepath even when min != max.
                if (can_slide_up) {
                    new_min = select(loop_var_expr <= loop_min, min_required, likely_if_innermost(prev_max_plus_one));
                    new_max = max_required;
                } else {
                    new_min = min_required;
                    new_max = select(loop_var_expr <= loop_min, max_required, likely_if_innermost(prev_min_minus_one));
                }
            }

            Expr early_stages_min_required = new_min;
            Expr early_stages_max_required = new_max;

            debug(3) << "Sliding " << func.name() << ", " << dim << "\n"
                     << "Pushing min up from " << min_required << " to " << new_min << "\n"
                     << "Shrinking max from " << max_required << " to " << new_max << "\n"
                     << "Adjusting loop_min from " << loop_min << " to " << new_loop_min << "\n"
                     << "Equation " << simplify(new_loop_min_eq) << "\n";

            // Now redefine the appropriate regions required
            if (can_slide_up) {
                replacements[prefix + dim + ".min"] = new_min;
            } else {
                replacements[prefix + dim + ".max"] = new_max;
            }

            for (size_t i = 0; i < func.updates().size(); i++) {
                string n = func.name() + ".s" + std::to_string(i) + "." + dim;
                replacements[n + ".min"] = Variable::make(Int(32), prefix + dim + ".min");
                replacements[n + ".max"] = Variable::make(Int(32), prefix + dim + ".max");
            }

            // Ok, we have a new min/max required and we're going to
            // rewrite all the lets that define bounds required. Now
            // we need to additionally expand the bounds required of
            // the last stage to cover values produced by stages
            // before the last one. Because, e.g., an intermediate
            // stage may be unrolled, expanding its bounds provided.
            if (!func.updates().empty()) {
                Box b = box_provided(op->body, func.name());
                if (can_slide_up) {
                    string n = prefix + dim + ".min";
                    Expr var = Variable::make(Int(32), n);
                    stmt = LetStmt::make(n, min(var, b[dim_idx].min), stmt);
                } else {
                    string n = prefix + dim + ".max";
                    Expr var = Variable::make(Int(32), n);
                    stmt = LetStmt::make(n, max(var, b[dim_idx].max), stmt);
                }
            }
            return stmt;
        } else if (!find_produce(op, func.name()) && new_loop_min.defined()) {
            // The producer might have expanded the loop before the min to warm
            // up the window. This consumer doesn't contain a producer that might
            // be part of the warmup, so guard it with an if to only run it on
            // the original loop bounds.
            Expr loop_var_expr = Variable::make(Int(32), loop_var);
            Expr orig_loop_min_expr = Variable::make(Int(32), loop_var + ".loop_min.orig");
            Expr guard = likely_if_innermost(loop_var_expr >= orig_loop_min_expr);

            // Put the if inside the consumer node, so semaphores end up outside the if.
            // TODO: This is correct, but it produces slightly suboptimal code: if we
            // didn't do this, the loop could likely be trimmed and the if simplified away.
            Stmt body = mutate(op->body);
            body = IfThenElse::make(guard, body);
            return ProducerConsumer::make(op->name, false, body);
        } else {
            return IRMutator::visit(op);
        }
    }

    Stmt visit(const For *op) override {
        // It's not safe to enter an inner loop whose bounds depend on
        // the var we're sliding over.
        Expr min = expand_expr(op->min, scope);
        Expr extent = expand_expr(op->extent, scope);
        if (is_const_one(extent)) {
            // Just treat it like a let
            Stmt s = LetStmt::make(op->name, min, op->body);
            s = mutate(s);
            // Unpack it back into the for
            const LetStmt *l = s.as<LetStmt>();
            internal_assert(l);
            return For::make(op->name, op->min, op->extent, op->for_type, op->device_api, l->body);
        } else if (is_monotonic(min, loop_var) != Monotonic::Constant ||
                   is_monotonic(extent, loop_var) != Monotonic::Constant) {
            debug(3) << "Not entering loop over " << op->name
                     << " because the bounds depend on the var we're sliding over: "
                     << min << ", " << extent << "\n";
            return op;
        } else {
            return IRMutator::visit(op);
        }
    }

    Stmt visit(const LetStmt *op) override {
        ScopedBinding<Expr> bind(scope, op->name, simplify(expand_expr(op->value, scope)));
        Stmt new_body = mutate(op->body);

        Expr value = op->value;

        map<string, Expr>::iterator iter = replacements.find(op->name);
        if (iter != replacements.end()) {
            value = iter->second;
            replacements.erase(iter);
        }

        if (new_body.same_as(op->body) && value.same_as(op->value)) {
            return op;
        } else {
            return LetStmt::make(op->name, value, new_body);
        }
    }

public:
    SlidingWindowOnFunctionAndLoop(Function f, string v, Expr v_min)
        : func(std::move(f)), loop_var(std::move(v)), loop_min(std::move(v_min)) {
    }

    Expr new_loop_min;
};

// Perform sliding window optimization for a particular function
class SlidingWindowOnFunction : public IRMutator {
    Function func;

    using IRMutator::visit;

    Stmt visit(const For *op) override {
        debug(3) << " Doing sliding window analysis over loop: " << op->name << "\n";

        Stmt new_body = op->body;

        std::string new_loop_name = op->name;

        Expr new_loop_min;
        Expr new_loop_extent;
        if (op->for_type == ForType::Serial ||
            op->for_type == ForType::Unrolled) {
            SlidingWindowOnFunctionAndLoop slider(func, op->name, op->min);
            new_body = slider.mutate(new_body);
            // We might have modified the loop min. If so, update the loop extent
            // to preserve the max.
            if (slider.new_loop_min.defined()) {
                new_loop_min = slider.new_loop_min;
                // We also need to rename the loop.
                new_loop_name += ".n";

                // The new loop interval is the new loop min to the old loop max.
                std::string loop_max_name = op->min.as<Variable>()->name;
                loop_max_name = loop_max_name.substr(0, loop_max_name.length() - 2) + "ax";
                Expr loop_max = Variable::make(Int(32), loop_max_name);
                new_loop_extent = loop_max - Variable::make(Int(32), new_loop_name + ".loop_min") + 1;
            }
        }

        Expr new_min = op->min;
        Expr new_extent = op->extent;
        if (new_loop_name != op->name) {
            // At this point, everything above is implemented by shadowing the old loop variable and related
            // lets. This isn't OK, so fix that here.
            new_min = Variable::make(Int(32), new_loop_name + ".loop_min");
            new_extent = Variable::make(Int(32), new_loop_name + ".loop_extent");
            std::map<string, Expr> renames = {
                {op->name, Variable::make(Int(32), new_loop_name)},
                {op->name + ".loop_extent", new_extent},
                {op->name + ".loop_min", new_min},
            };
            new_body = substitute(renames, new_body);
        }

        new_body = mutate(new_body);

        Stmt new_for;
        if (new_body.same_as(op->body) && new_loop_name == op->name && new_min.same_as(op->min) && new_extent.same_as(op->extent)) {
            new_for = op;
        } else {
            new_for = For::make(new_loop_name, new_min, new_extent, op->for_type, op->device_api, new_body);
        }

        if (new_loop_min.defined()) {
            Expr new_loop_max =
                Variable::make(Int(32), new_loop_name + ".loop_min") + Variable::make(Int(32), new_loop_name + ".loop_extent") - 1;
            new_for = LetStmt::make(new_loop_name + ".loop_max", new_loop_max, new_for);
            new_for = LetStmt::make(new_loop_name + ".loop_extent", new_loop_extent, new_for);
            new_for = LetStmt::make(new_loop_name + ".loop_min.orig", Variable::make(Int(32), new_loop_name + ".loop_min"), new_for);
            new_for = LetStmt::make(new_loop_name + ".loop_min", new_loop_min, new_for);
        }

        return new_for;
    }

public:
    SlidingWindowOnFunction(Function f)
        : func(std::move(f)) {
    }
};

// Perform sliding window optimization for all functions
class SlidingWindow : public IRMutator {
    const map<string, Function> &env;

    using IRMutator::visit;

    Stmt visit(const Realize *op) override {
        // Find the args for this function
        map<string, Function>::const_iterator iter = env.find(op->name);

        // If it's not in the environment it's some anonymous
        // realization that we should skip (e.g. an inlined reduction)
        if (iter == env.end()) {
            return IRMutator::visit(op);
        }

        // If the Function in question has the same compute_at level
        // as its store_at level, skip it.
        const FuncSchedule &sched = iter->second.schedule();
        if (sched.compute_level() == sched.store_level()) {
            return IRMutator::visit(op);
        }

        Stmt new_body = op->body;

        new_body = mutate(new_body);

        debug(3) << "Doing sliding window analysis on realization of " << op->name << "\n";
        new_body = SlidingWindowOnFunction(iter->second).mutate(new_body);

        if (new_body.same_as(op->body)) {
            return op;
        } else {
            return Realize::make(op->name, op->types, op->memory_type,
                                 op->bounds, op->condition, new_body);
        }
    }

public:
    SlidingWindow(const map<string, Function> &e)
        : env(e) {
    }
};

class AddLoopMinOrig : public IRMutator {
    using IRMutator::visit;

    Stmt visit(const For *op) {
        Stmt body = mutate(op->body);
        Expr min = mutate(op->min);
        Expr extent = mutate(op->extent);
        Stmt result = For::make(op->name, min, extent, op->for_type, op->device_api, body);
        result = LetStmt::make(op->name + ".loop_min.orig", Variable::make(Int(32), op->name + ".loop_min"), result);
        return result;
    }
};

}  // namespace

Stmt sliding_window(const Stmt &s, const map<string, Function> &env) {
    return SlidingWindow(env).mutate(AddLoopMinOrig().mutate(s));
}

}  // namespace Internal
}  // namespace Halide
