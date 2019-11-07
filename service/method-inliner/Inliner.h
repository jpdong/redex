/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <map>
#include <set>
#include <vector>

#include "CommonSubexpressionElimination.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationTransform.h"
#include "CopyPropagation.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IPConstantPropagationAnalysis.h"
#include "IRCode.h"
#include "LocalDce.h"
#include "MethodProfiles.h"
#include "PatriciaTreeSet.h"
#include "PriorityThreadPool.h"
#include "Resolver.h"

namespace inliner {

/*
 * Inline tail-called `callee` into `caller` at `pos`.
 *
 * NB: This is NOT a general-purpose inliner; it assumes that the caller does
 * not do any work after the call, so the only live registers are the
 * parameters to the callee. This allows it to do inlining by simply renaming
 * the callee's registers. The more general inline_method instead inserts
 * move instructions to map the caller's argument registers to the callee's
 * params.
 *
 * In general, use of this method should be considered deprecated. It is
 * currently only being used by the BridgePass because the insertion of
 * additional move instructions would confuse SynthPass, which looks for
 * exact sequences of instructions.
 */
void inline_tail_call(DexMethod* caller,
                      DexMethod* callee,
                      IRList::iterator pos);

/*
 * Inline `callee` into `caller` at `pos`.
 * This is a general-purpose inliner.
 */
void inline_method(IRCode* caller, IRCode* callee, IRList::iterator pos);

/*
 * Use the editable CFG instead of IRCode to do the inlining. Return true on
 * success.
 */
bool inline_with_cfg(DexMethod* caller_method,
                     DexMethod* callee_method,
                     IRInstruction* callsite);

} // namespace inliner

/**
 * What kind of caller-callee relationships the inliner should consider.
 */
enum MultiMethodInlinerMode {
  None,
  InterDex,
  IntraDex,
};

using CalleeCallerInsns = std::unordered_map<
    DexMethod*,
    std::unordered_map<DexMethod*, std::unordered_set<IRInstruction*>>>;

using ConstantArguments = constant_propagation::interprocedural::ArgumentDomain;

using InvokeConstantArguments =
    std::vector<std::pair<IRList::iterator, ConstantArguments>>;

struct InvokeConstantArgumentsAndDeadBlocks {
  InvokeConstantArguments invoke_constant_arguments;
  size_t dead_blocks{0};
};

using ConstantArgumentsOccurrences = std::pair<ConstantArguments, size_t>;

/**
 * Helper class to inline a set of candidates.
 * Take a set of candidates and a scope and walk all instructions in scope
 * to find and inline all calls to candidate.
 * A resolver is used to map a method reference to a method definition.
 * Not all methods may be inlined both for restriction on the caller or the
 * callee.
 * Perform inlining bottom up.
 */
class MultiMethodInliner {
 public:
  /**
   * We can do global inlining before InterDexPass, but after InterDexPass, we
   * can only inline methods within each dex. Set intra_dex to true if
   * inlining is needed after InterDex.
   */
  MultiMethodInliner(
      const std::vector<DexClass*>& scope,
      DexStoresVector& stores,
      const std::unordered_set<DexMethod*>& candidates,
      std::function<DexMethod*(DexMethodRef*, MethodSearch)> resolver,
      const inliner::InlinerConfig& config,
      MultiMethodInlinerMode mode = InterDex,
      const CalleeCallerInsns& true_virtual_callers = {},
      const std::unordered_map<const DexMethodRef*, method_profiles::Stats>&
          method_profile_stats = {});

  ~MultiMethodInliner() { invoke_direct_to_static(); }

  /**
   * attempt inlining for all candidates.
   */
  void inline_methods();

  /**
   * Return the count of unique inlined methods.
   */
  std::unordered_set<DexMethod*> get_inlined() const { return inlined; }

  bool for_speed() const { return !m_hot_methods.empty(); }

  /**
   * Inline callees in the caller if is_inlinable below returns true.
   */
  void inline_callees(DexMethod* caller,
                      const std::vector<DexMethod*>& callees);

  /**
   * Inline callees in the given instructions in the caller, if is_inlinable
   * below returns true.
   */
  void inline_callees(DexMethod* caller,
                      const std::unordered_set<IRInstruction*>& insns);

  /**
   * Return true if the callee is inlinable into the caller.
   * The predicates below define the constraints for inlining.
   * Providing an instrucion is optional, and only used for logging.
   */
  bool is_inlinable(DexMethod* caller,
                    DexMethod* callee,
                    const IRInstruction* insn,
                    size_t estimated_insn_size);

 private:
  void caller_inline(DexMethod* caller,
                     const std::vector<DexMethod*>& nonrecursive_callees);

  using CallerNonrecursiveCalleesByStackDepth = std::unordered_map<
      size_t,
      std::vector<std::pair<DexMethod*, std::vector<DexMethod*>>>>;

  /**
   * Determine order in which to inline.
   * Recurse in a callee if that has inlinable candidates of its own.
   * Inlining is bottom up.
   */
  size_t compute_caller_nonrecursive_callees_by_stack_depth(
      DexMethod* caller,
      const std::vector<DexMethod*>& callees,
      sparta::PatriciaTreeSet<DexMethod*> call_stack,
      std::unordered_map<DexMethod*, size_t>* visited,
      CallerNonrecursiveCalleesByStackDepth*
          caller_nonrecursive_callees_by_stack_depth);

  void inline_inlinables(
      DexMethod* caller,
      const std::vector<std::pair<DexMethod*, IRList::iterator>>& inlinables);

  /**
   * Return true if the method is related to enum (java.lang.Enum and derived).
   * Cannot inline enum methods because they can be called by code we do
   * not own.
   */
  bool is_blacklisted(const DexMethod* callee);

  bool caller_is_blacklisted(const DexMethod* caller);

  /**
   * Return true if the callee contains external catch exception types
   * which are not public.
   */
  bool has_external_catch(const DexMethod* callee);

  /**
   * Return true if the callee contains certain opcodes that are difficult
   * or impossible to inline.
   * Some of the opcodes are defined by the methods below.
   * When returning false, some methods might have been added to make_static.
   */
  bool cannot_inline_opcodes(const DexMethod* caller,
                             const DexMethod* callee,
                             const IRInstruction* invk_insn,
                             std::vector<DexMethod*>* make_static);

  /**
   * Return true if inlining would require a method called from the callee
   * (candidate) to turn into a virtual method (e.g. private to public).
   * When returning false, a method might have been added to make_static.
   */
  bool create_vmethod(IRInstruction* insn,
                      const DexMethod* callee,
                      const DexMethod* caller,
                      std::vector<DexMethod*>* make_static);

  /**
   * Return true if a callee contains an invoke super to a different method
   * in the hierarchy.
   * invoke-super can only exist within the class the call lives in.
   */
  bool nonrelocatable_invoke_super(IRInstruction* insn);

  /**
   * Return true if the callee contains a call to an unknown virtual method.
   * We cannot determine the visibility of the method invoked and thus
   * we cannot inline as we could cause a verification error if the method
   * was package/protected and we move the call out of context.
   */
  bool unknown_virtual(IRInstruction* insn);

  /**
   * Return true if the callee contains an access to an unknown field.
   * We cannot determine the visibility of the field accessed and thus
   * we cannot inline as we could cause a verification error if the field
   * was package/protected and we move the access out of context.
   */
  bool unknown_field(IRInstruction* insn);

  /**
   * return true if `insn` is
   *   sget android.os.Build.VERSION.SDK_INT
   */
  bool check_android_os_version(IRInstruction* insn);

  /**
   * Return true if a caller is in a DEX in a store and any opcode in callee
   * refers to a DexMember in a different store .
   */
  bool cross_store_reference(const DexMethod* context);

  bool is_estimate_over_max(uint64_t estimated_insn_size,
                            const DexMethod* callee,
                            uint64_t max);

  /**
   * Some versions of ART (5.0.0 - 5.0.2) will fail to verify a method if it
   * is too large. See https://code.google.com/p/android/issues/detail?id=66655.
   *
   * Right now we only check for the number of instructions, but there are
   * other constraints that might be worth looking into, e.g. the number of
   * registers.
   */
  bool caller_too_large(DexType* caller_type,
                        size_t estimated_caller_size,
                        const DexMethod* callee);

  /**
   * Return whether the callee should be inlined into the caller. This differs
   * from is_inlinable in that the former is concerned with whether inlining is
   * possible to do correctly at all, whereas this is concerned with whether the
   * inlining is beneficial for size / performance.
   *
   * This method does *not* need to return a subset of is_inlinable. We will
   * only inline callsites that pass both should_inline and is_inlinable.
   *
   * Note that this filter will only be applied when inlining is initiated via
   * a call to `inline_methods()`, but not if `inline_callees()` is invoked
   * directly.
   */
  bool should_inline(const DexMethod* callee);

  /**
   * should_inline_fast will return true for a subset of methods compared to
   * should_inline. should_inline_fast can be evaluated much more quickly, as it
   * doesn't need to peek into the callee code.
   */
  bool should_inline_fast(const DexMethod* callee);

  /**
   * We want to avoid inlining a large method with many callers as that would
   * bloat the bytecode.
   */
  bool too_many_callers(const DexMethod* callee);

  /**
   * Estimate inlined cost for a single invocation of a method.
   */
  size_t get_inlined_cost(const DexMethod* callee);

  /**
   * Staticize required methods (stored in `m_make_static`) and update
   * opcodes accordingly.
   *
   * NOTE: It only needs to be called once after inlining. Since it is called
   *       from the destructor, there is no need to manually call it.
   */
  void invoke_direct_to_static();

  /**
   * For all (reachable) invoke instructions in a given method, collect
   * information about their arguments, i.e. whether particular arguments
   * are constants.
   */
  boost::optional<InvokeConstantArgumentsAndDeadBlocks>
  get_invoke_constant_arguments(DexMethod* caller,
                                const std::vector<DexMethod*>&);

  /**
   * Build up constant-arguments information for all invoked methods.
   */
  void compute_callee_constant_arguments();

  /**
   * Initiate post-processing a method asynchronously.
   */
  void async_postprocess_method(DexMethod* method);

  /**
   * Post-processing a method synchronously.
   */
  void postprocess_method(DexMethod* method);

  /**
   * Shrink a method (run constant-prop, cse, copy-prop, local-dce)
   * synchronously.
   */
  void shrink_method(DexMethod* method);

  /**
   * For callers waiting for callees to become ready, decrement their wait
   * counter, and if zero, initiate inlining and postprocessing.
   */
  void decrement_caller_wait_counts(const std::vector<DexMethod*>& callers);

  /**
   * If a callee has been registered for delayed shrinking, decrement the wait
   * counter, and if zero, initiate shrinking asynchronously.
   */
  void decrement_delayed_shrinking_callee_wait_counts(
      const std::vector<DexMethod*>& callees);

  /**
   * Whether inline_inlinables needs to deconstruct the caller's and callees'
   * code.
   */
  bool inline_inlinables_need_deconstruct(DexMethod* method);

  /**
   * Execute asynchronously using a method's priority.
   */
  void async_prioritized_method_execute(DexMethod* method,
                                        std::function<void()> f);

 private:
  /**
   * Resolver function to map a method reference to a method definition.
   */
  std::function<DexMethod*(DexMethodRef*, MethodSearch)> resolver;

  /**
   * Checker for cross stores contaminations.
   */
  XStoreRefs xstores;

  /**
   * Inlined methods.
   */
  std::unordered_set<DexMethod*> inlined;

  //
  // Maps from callee to callers and reverse map from caller to callees.
  // Those are used to perform bottom up inlining.
  //
  std::map<const DexMethod*, std::vector<DexMethod*>, dexmethods_comparator>
      callee_caller;
  // this map is ordered in order that we inline our methods in a repeatable
  // fashion so as to create reproducible binaries
  std::map<DexMethod*, std::vector<DexMethod*>, dexmethods_comparator>
      caller_callee;

  std::unordered_map<DexMethod*, std::unordered_map<IRInstruction*, DexMethod*>>
      caller_virtual_callee;

  // Cache of the inlined costs of each method after all its eligible callsites
  // have been inlined.
  mutable ConcurrentMap<const DexMethod*, boost::optional<size_t>>
      m_inlined_costs;

  /**
   * For all (reachable) invoked methods, list of constant arguments
   */
  mutable std::unordered_map<const DexMethod*,
                             std::vector<ConstantArgumentsOccurrences>>
      m_callee_constant_arguments;

  // Cache of whether all callers of a callee are in the same class.
  mutable ConcurrentMap<const DexMethod*, boost::optional<bool>>
      m_callers_in_same_class;

  // Priority thread pool to handle parallel processing of methods, either
  // shrinking initially / after inlining into them, or even to inline in
  // parallel. By default, parallelism is disabled num_threads = 0).
  PriorityThreadPool m_async_method_executor{0};

  // For parallel execution, priorities for methods, to minimize waiting.
  std::unordered_map<const DexMethod*, int> m_async_callee_priorities;

  // For parallel execution, callee-callers relationships. The induced tree
  // has been pruned of recursive relationships.
  std::unordered_map<const DexMethod*, std::vector<DexMethod*>>
      m_async_callee_callers;

  // For parallel execution, caller-callees relationships. The induced tree
  // has been pruned of recursive relationships.
  std::unordered_map<const DexMethod*, std::vector<DexMethod*>>
      m_async_caller_callees;

  // For parallel execution, number of remaining callees any given caller is
  // still waiting for.
  ConcurrentMap<const DexMethod*, size_t> m_async_caller_wait_counts;

  // For parallel execution, number of remaining callers any given delayed
  // shrinking callee is still waiting for.
  ConcurrentMap<const DexMethod*, size_t>
      m_async_delayed_shrinking_callee_wait_counts;

  // Whether any of const-prop/cs/copy-prop/local-dce are enabled.
  bool m_shrinking_enabled{0};

  // When mutating shared state, except info, while inlining in parallel
  std::mutex m_mutex;

  // When mutating info while inlining in parallel
  std::mutex m_info_mutex;

  // Cache for should_inline function
  ConcurrentMap<const DexMethod*, boost::optional<bool>> m_should_inline;

  constant_propagation::Transform::Stats m_const_prop_stats;
  cse_impl::Stats m_cse_stats;
  copy_propagation_impl::Stats m_copy_prop_stats;
  LocalDce::Stats m_local_dce_stats;
  size_t m_methods_shrunk{0};

  // When mutating service stats while inlining in parallel
  std::mutex m_stats_mutex;

 private:
  /**
   * Info about inlining.
   */
  struct InliningInfo {
    size_t calls_inlined{0};
    size_t recursive{0};
    size_t max_call_stack_depth{0};
    size_t not_found{0};
    size_t blacklisted{0};
    size_t throws{0};
    size_t multi_ret{0};
    size_t need_vmethod{0};
    size_t invoke_super{0};
    size_t write_over_ins{0};
    size_t escaped_virtual{0};
    size_t known_public_methods{0};
    size_t unresolved_methods{0};
    size_t non_pub_virtual{0};
    size_t escaped_field{0};
    size_t non_pub_field{0};
    size_t non_pub_ctor{0};
    size_t cross_store{0};
    size_t caller_too_large{0};
    size_t constant_invoke_callers_analyzed{0};
    size_t constant_invoke_callers_unreachable_blocks{0};
    size_t constant_invoke_callees_analyzed{0};
    size_t constant_invoke_callees_unreachable_blocks{0};
    size_t waited_seconds{0};
    int critical_path_length{0};
  };
  InliningInfo info;

  const std::vector<DexClass*>& m_scope;

  const inliner::InlinerConfig& m_config;

  std::unordered_set<DexMethod*> m_make_static;

  const MultiMethodInlinerMode m_mode;

  const std::unordered_set<const DexMethodRef*> m_hot_methods;

  const std::unordered_set<DexMethodRef*> m_pure_methods;

  std::unique_ptr<cse_impl::SharedState> m_cse_shared_state;

 public:
  const InliningInfo& get_info() { return info; }

  const constant_propagation::Transform::Stats& get_const_prop_stats() {
    return m_const_prop_stats;
  }
  const cse_impl::Stats& get_cse_stats() { return m_cse_stats; }
  const copy_propagation_impl::Stats& get_copy_prop_stats() {
    return m_copy_prop_stats;
  }
  const LocalDce::Stats& get_local_dce_stats() { return m_local_dce_stats; }
  size_t get_methods_shrunk() { return m_methods_shrunk; }
  size_t get_callers() { return m_async_caller_wait_counts.size(); }
  size_t get_delayed_shrinking_callees() {
    return m_async_delayed_shrinking_callee_wait_counts.size();
  }
};
