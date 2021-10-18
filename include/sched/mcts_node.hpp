#pragma once

#include "schedule.hpp"
#include "benchmarker.hpp"

namespace mcts {


template <typename Strategy>
struct Node {

    using Context = typename Strategy::Context;
    using State = typename Strategy::State;

    Node *parent_;
    std::vector<Node> children_;
    std::shared_ptr<CpuNode> op_;
    bool expanded_;
    bool fullyVisited_; // if this subtree fully expanded
    float valueEstimate_; // an estimate of this node's value if it doesn't have enough playouts
    size_t n_; // # of playouts

    // state required for whatever the strategy is
    State state_;

    Node(const std::shared_ptr<CpuNode> &op) 
        : parent_(nullptr), op_(op), expanded_(false), fullyVisited_(false),
        valueEstimate_(std::numeric_limits<float>::infinity()), // estimate an infinite value before a child is visited
        n_(0) {}

    // true if node can't have children
    bool is_terminal(const Graph<CpuNode> &g);
    bool is_leaf() const {
        if (children_.empty()) {
            return true;
        }
        for (const auto &child : children_) {
            if (child.state_.times.empty()) {
                return true;
            }
        }
        return false;
    }

    // select successive child nodes until a leaf L is reached
    Node &select(Context &ctx, const Graph<CpuNode> &g);

    // create unexanded children for this node
    Node &expand(const Context &ctx, const Graph<CpuNode> &g);

    // Measure a random ordering from this node
    std::vector<std::shared_ptr<CpuNode>> get_simulation_order(const Graph<CpuNode> &g);

    // backpropagate results up the tree.
    // invokes Strategy::backprop
    void backprop(Context &ctx, const Benchmark::Result &br);
};

template <typename Strategy>
void Node<Strategy>::backprop(Context &ctx, const Benchmark::Result &br) {
    ++n_; // additional playout

    if (children_.empty()) {
        if (expanded_) {
            fullyVisited_ = expanded_;
            STDERR(op_->name() << " fully visisted (no children)");
        }
    } else { // if all children are visited
        bool allChildrenVisited = true;
        for (Node &child : children_) {
            allChildrenVisited = allChildrenVisited && child.fullyVisited_;
        }
        if (allChildrenVisited) {
            fullyVisited_ = true;
            STDERR(op_->name() << " fully visisted (all children explored)");
        }
    }


    Strategy::backprop(ctx, *this, br);
    if (parent_) {
        parent_->backprop(ctx, br);
    }
}

/* select successive child nodes until a leaf is reached
   a leaf is a node that has a child from which no simulation has been played
*/
template <typename Strategy>
Node<Strategy> &Node<Strategy>::select(Context &ctx, const Graph<CpuNode> &g) {
    if (is_leaf() || is_terminal(g)) {
        return *this;
    } else {

        STDERR(ctx);

        // ubc of each child
        std::vector<float> ucts;
        for (const auto &child : children_) {

            // value of child
            const float exploit = Strategy::select(ctx, *this, child);
            const float c = std::sqrt(2.0f);

            // value of exploring this child
            float explore;

            if (child.fullyVisited_) { // penalize children with no more orderings to visit
                explore = -std::numeric_limits<float>::infinity();
            } else if (1 > child.n_) {
                explore = c * std::sqrt(std::log(n_)/1);
            } else {
                explore = c * std::sqrt(std::log(n_)/child.n_);
            }

            const float uct = exploit + explore;

            STDERR(
            child.op_->name() 
            << ": n=" << child.state_.times.size() 
            << " explore=" << explore 
            << " exploit=" << exploit
            << " minT=" << *child.state_.times.begin()
            << " maxT=" << child.state_.times.back()
            );
            ucts.push_back(uct);

        }

        // argmax(ucts)
        // if it's a tie, return a random one since the children
        // are in no particular order
        size_t im = 0;
        {
            float m = -1 * std::numeric_limits<float>::infinity();
            for (size_t i = 0; i < ucts.size(); ++i) {
                if (ucts[i] > m) {
                    m = ucts[i];
                    im = i;
                }
            }
            std::vector<size_t> choices;
            for (size_t i = 0; i < ucts.size(); ++i) {
                if (ucts[i] == m) {
                    choices.push_back(i);
                }
            }
            im = choices[rand() % choices.size()];

            STDERR("selected " << children_[im].op_->name() << " uct=" << m);
        }


        return children_[im].select(ctx, g);
    }
}

template <typename Strategy>
Node<Strategy> &Node<Strategy>::expand(const Context &, const Graph<CpuNode> &g) {
    typedef std::shared_ptr<CpuNode> Op;

    // if (is_terminal(g)) {
    //     return *this;
    // }

    // create child nodes if needed
    if (!expanded_) {
        // get the path we took to be here
        std::vector<Op> path;
        Node *current = this;
        while(current) {
            path.push_back(current->op_);
            current = current->parent_;
        }

        // make sure each successor of every node in the path appears
        // exactly once in the frontier list
        std::vector<Op> frontier;
        for (const auto &op : path ) {
            for (const auto &child : g.succs_.at(op)) {
                if (frontier.end() == std::find(frontier.begin(), frontier.end(), child)) {
                    frontier.push_back(child);
                }
            }
        }

        // remove all ops in the frontier that we've already done
        for (const Op &op : path) {
            while (true) {
                auto it = std::find(frontier.begin(), frontier.end(), op);
                if (it != frontier.end()) {
                    frontier.erase(it);
                } else {
                    break;
                }
            }
        }

        // remove all ops in the frontier that have a predecessor that's not
        // in the path
        {
            bool changed = true;
            while(changed) {
                changed = false;
                for (auto fi = frontier.begin(); fi != frontier.end(); ++fi) {
                    for (const auto &pred : g.preds_.at(*fi)) {
                        // predecessor is not in the path
                        if (path.end() == std::find(path.begin(), path.end(), pred)) {
                            frontier.erase(fi);
                            changed = true;
                            goto endloop;
                        }
                    }
                }
                endloop: ;
            }
        }

        // create all child nodes
        for (const Op &op : frontier) {
            Node node(op);
            node.parent_ = this;
            children_.push_back(node);
        }
        STDERR("expanded " << children_.size() << " children");

        // mark node expanded
        expanded_ = true;
    }

    // chose a child node to return
    if (children_.empty()) {
        return *this; // terminal
    } else {

        // TODO: don't automatically prioritize unplayed nodes
        // give them the value of their parents and use UCTS

        // random
        // return children_[rand() % children_.size()];

        // first unplayed node
        for (auto &child : children_) {
            if (child.state_.times.empty()) {
                return child;
            }
        }
        // if all children have been played, this is not a leaf node
        THROW_RUNTIME("unreachable");
    }
}


template <typename Strategy>
std::vector<std::shared_ptr<CpuNode>> Node<Strategy>::get_simulation_order(const Graph<CpuNode> &g) {
    typedef std::shared_ptr<CpuNode> Op;

    // get the path we took to be here
    std::vector<Op> path;
    Node *current = this;
    while(current) {
        path.push_back(current->op_);
        current = current->parent_;
    }
    std::reverse(path.begin(), path.end());
    {
        std::string s;
        for (const auto &op : path) {
            s += op->name();
            s += ", ";
        }
        STDERR("path is: " << s);
    }


    // choose a random traversal of the remaining nodes
    std::vector<Op> frontier;
    // add all successors in the path that have not already been visited
    // and have all predecessors complete
    {
        // add one copy of each successor to all nodes in the path
        for (const auto &op : path ) {
            for (const auto &child : g.succs_.at(op)) {

                bool unique = (frontier.end() == std::find(frontier.begin(), frontier.end(), child));
                bool notDone = (path.end() == std::find(path.begin(), path.end(), child));
                bool predsDone = true;
                for (const auto &pred : g.preds_.at(child)) {
                    if (path.end() == std::find(path.begin(), path.end(), pred)) {
                        predsDone = false;
                        break;
                    }
                }
                // STDERR(child->name() << " " << unique << " " << notDone << " " << predsDone);
                if (unique && notDone && predsDone) {
                    frontier.push_back(child);
                }
            }
        }
    }

    STDERR("random path...");
    while(!frontier.empty()) {
        // choose a random node that's up next
        size_t ii = rand() % frontier.size();
        auto op = frontier[ii];
    
        // add to path
        path.push_back(op);
        // STDERR("current path is ...");
        // for (auto & e : path) {
        //     STDERR(e->name());
        // }

        // add its successors if they're not in the path, they haven't been done,
        // and its preds are done
        for (const auto &succ : g.succs_.at(op)) {
            bool unique = (frontier.end() == std::find(frontier.begin(), frontier.end(), succ));
            bool notDone = (path.end() == std::find(path.begin(), path.end(), succ));
            bool predsDone = true;
            for (const auto &pred : g.preds_.at(succ)) {
                if (path.end() == std::find(path.begin(), path.end(), pred)) {
                    predsDone = false;
                    break;
                }
            }
            // STDERR(succ->name() << " " << unique << " " << notDone << " " << predsDone);
            if (unique && notDone && predsDone) {
                frontier.push_back(succ);
            }
        }

        // erase from frontier
        frontier.erase(frontier.begin() + ii);
    }

    {
        std::string s;
        for (const auto &op : path) {
            s += op->name();
            s += ", ";
        }
        STDERR("random path is: " << s);
    }

    return path;
}


template<typename Strategy>
bool Node<Strategy>::is_terminal(const Graph<CpuNode> &g) {
    return g.succs_.at(op_).empty();
}

} // namespace mcts