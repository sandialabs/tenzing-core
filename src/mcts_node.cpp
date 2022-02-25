/* Copyright 2022 National Technology & Engineering Solutions of Sandia, LLC (NTESS). Under the
 * terms of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this
 * software.
 */

#include "sched/mcts_node.hpp"

#include "sched/operation.hpp"
#include "sched/cuda/ops_cuda.hpp"
#include "sched/sequence.hpp"
#include "sched/event_synchronizer.hpp"


/* return the frontier of nodes from g given already-traversed nodes
    g may or may not include:
      synchronization
      resource assignments

    The next possible operations are those that have:
      all predecessor issued
      are not in the completed operations

    For those operations, all possible resource assignments can be considered
    The next executable states are those BoundOps that have
        the resources of all predecessors synced

    If a predecessor is issued but its resources are not synced,
    the fronter can contain the corresponding sync operation instead of the operation itself

    The next executable states are those operations that have
      all predecessors issued
      all resources for those predecessors

    FIXME: this adds synchronizations for all possible stream assignments of future
    operations, of which only one is selected, usually inserting lots of extra syncs

*/
std::vector<std::shared_ptr<BoundOp>>
get_frontier(Platform &plat, const Graph<OpBase> &g,
             const Sequence<BoundOp> &completed) {
  typedef EventSynchronizer Synchronizer;
  /*
  find candidate operations for the frontier
      all predecessors in `completed`
      is not itself in `completed`
  */
  {
    std::stringstream ss;
    ss << "frontier for state: ";
    for (const auto &op : completed) {
      ss << op->desc() << ",";
    }
    STDERR(ss.str());
  }

  STDERR("consider ops with >= 1 pred completed...");
  std::vector<std::shared_ptr<OpBase>> onePredCompleted;
  for (const auto &cOp : completed) {
    STDERR("...succs of " << cOp->desc() << " (@" << cOp.get() << ")");

    // some nodes in the path will not be in the graph (inserted syncs)
    // other nodes in the path are bound versions of that in the graph

    auto it = g.succs_find_or_find_unbound(cOp);
    if (g.succs_.end() != it) {

      // all successors of a completed op have at least one pred completed
      for (const auto &succ : it->second) {
        // don't add duplicates
        if (onePredCompleted.end() ==
            std::find(onePredCompleted.begin(), onePredCompleted.end(), succ)) {
          onePredCompleted.push_back(succ);
        }
      }
    }
  }

  {
    std::stringstream ss;
    ss << "one pred completed: ";
    for (const auto &op : onePredCompleted) {
      ss << op->desc() << ",";
    }
    STDERR(ss.str());
  }

  STDERR("reject ops already done or with incomplete preds...");
  std::vector<std::shared_ptr<OpBase>> candidates;
  for (const auto &cOp : onePredCompleted) {
    // reject ops that we've already done
    if (completed.contains_unbound(cOp)) {
      STDERR(cOp->name() << " already done");
      continue;
    }

    // reject ops that all preds are not done
    bool allPredsCompleted = true;
    for (const auto &pred : g.preds_.at(cOp)) {
      if (!completed.contains_unbound(pred)) {
        STDERR(cOp->name() << " missing pred " << pred->name());
        allPredsCompleted = false;
        break;
      }
    }
    if (!allPredsCompleted) {
      STDERR(cOp->name() << " missing a pred");
      continue;
    }
    candidates.push_back(cOp);
  }

  {
    std::stringstream ss;
    ss << "preds complete AND not done: ";
    for (const auto &op : candidates) {
      ss << op->desc() << ",";
    }
    STDERR(ss.str());
  }

  std::vector<std::shared_ptr<BoundOp>> frontier;

  STDERR("generate frontier from candidates...");
  // candidates may or may not be assigned to resources
  // get the viable resource assignments for the candidates
  for (const auto &candidate : candidates) {
    STDERR("candidate " << candidate->desc() << "...");
    std::vector<std::shared_ptr<BoundOp>> bounds = make_platform_variations(plat, candidate);
    STDERR("...got " << bounds.size() << " platform variations");

    for (const std::shared_ptr<BoundOp> &bound : bounds) {
      // if the candidate is already synchronized with its preds, it can be added to the frontier
      if (Synchronizer::is_synced(bound, g, completed)) {
        STDERR("variation of " << bound->desc() << " is synced");
        frontier.push_back(bound);
      } else { // otherwise synchronizers should be added instead
        STDERR("variation of " << bound->desc() << " is not synced with preds");
        std::vector<std::shared_ptr<BoundOp>> syncs = Synchronizer::make_syncs(bound, g, completed);
        STDERR("adding synchronizers for " << bound->desc() << " to frontier:");
        for (const auto &sync : syncs) {
          STDERR(sync->desc());
          frontier.push_back(sync);
        }
      }
    }
  }

  keep_uniques(frontier);
  return frontier;
}

#if 0
std::vector<std::shared_ptr<BoundOp>>
mcts::get_graph_frontier(Platform &plat, const Graph<OpBase> &g,
                         const Sequence<BoundOp> &completed, bool quiet) {

  /*
  find candidate operations for the frontier
      all predecessors in `completed`
      is not itself in `completed`
  */
  {
    std::stringstream ss;
    ss << "frontier for state: ";
    for (const auto &op : completed) {
      ss << op->desc() << ",";
    }
    STDERR(ss.str());
  }

  STDERR("consider ops with >= 1 pred completed...");
  std::vector<std::shared_ptr<OpBase>> onePredCompleted;
  for (const auto &cOp : completed) {
    if (!quiet)
      STDERR("...succs of " << cOp->desc() << " (@" << cOp.get() << ")");

    // some nodes in the path will not be in the graph (inserted syncs)
    // other nodes in the path are bound versions of that in the graph

    auto it = g.succs_find_or_find_unbound(cOp);
    if (g.succs_.end() != it) {

      // all successors of a completed op have at least one pred completed
      for (const auto &succ : it->second) {
        // don't add duplicates
        if (onePredCompleted.end() ==
            std::find(onePredCompleted.begin(), onePredCompleted.end(), succ)) {
          onePredCompleted.push_back(succ);
        }
      }
    }
  }

  {
    std::stringstream ss;
    ss << "one pred completed: ";
    for (const auto &op : onePredCompleted) {
      ss << op->desc() << ",";
    }
    STDERR(ss.str());
  }

  STDERR("reject ops already done or with incomplete preds...");
  std::vector<std::shared_ptr<OpBase>> candidates;
  for (const auto &cOp : onePredCompleted) {
    // reject ops that we've already done
    if (completed.contains_unbound(cOp)) {
      if (!quiet)
        STDERR(cOp->name() << " already done");
      continue;
    }

    // reject ops that all preds are not done
    bool allPredsCompleted = true;
    for (const auto &pred : g.preds_.at(cOp)) {
      if (!completed.contains_unbound(pred)) {
        STDERR(cOp->desc() << " missing pred " << pred->desc());
        allPredsCompleted = false;
        break;
      }
    }
    if (!allPredsCompleted) {
      STDERR(cOp->name() << " missing a pred");
      continue;
    }
    candidates.push_back(cOp);
  }

  {
    std::stringstream ss;
    ss << "preds complete AND not done: ";
    for (const auto &op : candidates) {
      ss << op->desc() << ",";
    }
    STDERR(ss.str());
  }

  // create all possible platform assignments of the graph frontier
  std::vector<std::shared_ptr<BoundOp>> result;
  for (const std::shared_ptr<OpBase> &candidate : candidates) {
    std::vector<std::shared_ptr<BoundOp>> bounds = make_platform_variations(plat, candidate);
    result.insert(result.end(), bounds.begin(), bounds.end());
  }

  return result;
}
#endif

/*
(2)
return a copy of g with an unbound version of op replaced with op
*/
Graph<OpBase> mcts::bind_unbound_vertex(const Graph<OpBase> &g,
                                        const std::shared_ptr<BoundOp> &op) {
  Graph<OpBase> gp = g; // g'
  if (!gp.contains(op)) {
    if (auto bgo = std::dynamic_pointer_cast<BoundGpuOp>(op)) {
      STDERR("replace " << bgo->unbound() << " with " << op->desc());
      gp.replace(bgo->unbound(), op);
    }
  }

  if (!gp.contains(op)) {
    THROW_RUNTIME("");
  }
  return gp;
}

/*
(3)
considering the `completed` so far, the graph, and the platform,
return all synchronizations that are needed before op can actually be
appended to the completed
return {} if none needed
*/
std::vector<std::shared_ptr<BoundOp>>
mcts::get_syncs_before_op(const Graph<OpBase> &g,
                          const Sequence<BoundOp> &completed,
                          const std::shared_ptr<BoundOp> &op) {
  typedef EventSynchronizer Synchronizer;

  std::vector<std::shared_ptr<BoundOp>> syncs;

  if (Synchronizer::is_synced(op, g, completed)) {
    STDERR(op->desc() << " is synced");
  } else { // otherwise synchronizers should be added
    STDERR(op->desc() << " is not synced with preds");
    syncs = Synchronizer::make_syncs(op, g, completed, true);
    {
      std::stringstream ss;
      ss << "generated synchronizers for " << op->desc() << ":";
      for (const auto &sync : syncs) {
        ss << sync->desc() << ", ";
      }
      STDERR(ss.str());
    }
  }
  return syncs;
}