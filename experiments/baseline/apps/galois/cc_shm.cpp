// Connected components from Galois's
// lonestar/analytics/cpu/connected-components/ConnectedComponents.cpp, with a
// driver that reads a symmetric graph in Galois's .gr format and prints:
//   load_s <seconds>
//   round <r> <seconds>      one line per timed round, after one untimed warm-up
//   iterations <count>       per timed round, label propagation only
//   components <count>
//
// Usage: cc_shm <graph.sgr> -symmetricGraph -algo=LabelProp|Afforest [-runs R]
//        [-t T]

/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#include "galois/AtomicHelpers.h"
#include "galois/Galois.h"
#include "galois/Reduction.h"
#include "galois/UnionFind.h"
#include "galois/gstl.h"
#include "galois/graphs/LCGraph.h"
#include "Lonestar/BoilerPlate.h"

#include "llvm/Support/CommandLine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>

const char* name = "Connected Components";
const char* desc = "Computes the connected components of a graph";

namespace cll = llvm::cl;

enum Algo { labelProp, afforest };

static cll::opt<std::string>
    inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<Algo>
    algo("algo", cll::desc("Choose an algorithm:"),
         cll::values(clEnumValN(Algo::labelProp, "LabelProp",
                                "Using label propagation algorithm"),
                     clEnumValN(Algo::afforest, "Afforest",
                                "Using Afforest sampling")),
         cll::Required);
static cll::opt<int> numRuns("runs", cll::desc("Timed rounds after the warm-up"),
                             cll::init(3));
static cll::opt<uint32_t> NEIGHBOR_SAMPLES(
    "vns", cll::desc("Edges per vertex to process initially (default 2)"),
    cll::init(2));
static cll::opt<uint32_t> COMPONENT_SAMPLES(
    "lcs",
    cll::desc("Samples to approximate the largest component (default 1024)"),
    cll::init(1024));

const unsigned int LABEL_INF = std::numeric_limits<unsigned int>::max();

struct LabelPropAlgo {
  struct LNode {
    std::atomic<unsigned int> comp_current;
    unsigned int comp_old;
  };

  using Graph =
      galois::graphs::LC_CSR_Graph<LNode, void>::with_no_lockable<true>::type;
  using GNode = Graph::GraphNode;

  void reset(Graph& graph) {
    galois::do_all(
        galois::iterate(graph),
        [&](const GNode& src) {
          LNode& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
          sdata.comp_current = src;
          sdata.comp_old     = LABEL_INF;
        },
        galois::no_stats(), galois::steal());
  }

  unsigned operator()(Graph& graph) {
    unsigned iterations = 0;
    galois::GReduceLogicalOr changed;
    do {
      ++iterations;
      changed.reset();
      galois::do_all(
          galois::iterate(graph),
          [&](const GNode& src) {
            LNode& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
            if (sdata.comp_old > sdata.comp_current) {
              sdata.comp_old = sdata.comp_current;

              changed.update(true);

              for (auto e : graph.edges(src, galois::MethodFlag::UNPROTECTED)) {
                GNode dst              = graph.getEdgeDst(e);
                auto& ddata            = graph.getData(dst);
                unsigned int label_new = sdata.comp_current;
                galois::atomicMin(ddata.comp_current, label_new);
              }
            }
          },
          galois::disable_conflict_detection(), galois::steal(),
          galois::loopname("LabelPropAlgo"));
    } while (changed.reduce());
    return iterations;
  }

  uint64_t components(Graph& graph) {
    galois::GAccumulator<uint64_t> count;
    galois::do_all(
        galois::iterate(graph),
        [&](const GNode& src) {
          if (graph.getData(src, galois::MethodFlag::UNPROTECTED)
                  .comp_current == src) {
            count += 1;
          }
        },
        galois::no_stats());
    return count.reduce();
  }
};

template <typename component_type, typename Graph>
component_type approxLargestComponent(Graph& graph) {
  using map_type = std::unordered_map<
      component_type, int, std::hash<component_type>,
      std::equal_to<component_type>,
      galois::gstl::Pow2Alloc<std::pair<const component_type, int>>>;
  using pair_type = std::pair<component_type, int>;

  map_type comp_freq(COMPONENT_SAMPLES);
  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<uint32_t> dist(0, graph.size() - 1);
  for (uint32_t i = 0; i < COMPONENT_SAMPLES; i++) {
    auto& ndata = graph.getData(dist(rng), galois::MethodFlag::UNPROTECTED);
    comp_freq[ndata.component()]++;
  }

  auto most_frequent =
      std::max_element(comp_freq.begin(), comp_freq.end(),
                       [](const pair_type& a, const pair_type& b) {
                         return a.second < b.second;
                       });

  return most_frequent->first;
}

struct AfforestAlgo {
  struct NodeData : public galois::UnionFindNode<NodeData> {
    using component_type = NodeData*;

    NodeData() : galois::UnionFindNode<NodeData>(const_cast<NodeData*>(this)) {}
    NodeData(const NodeData& o)
        : galois::UnionFindNode<NodeData>(o.m_component) {}

    component_type component() { return this->get(); }

    void reset() { m_component.store(this, std::memory_order_relaxed); }

    void link(NodeData* b) {
      NodeData* a = m_component.load(std::memory_order_relaxed);
      b           = b->m_component.load(std::memory_order_relaxed);
      while (a != b) {
        if (a < b)
          std::swap(a, b);
        NodeData* ac = a->m_component.load(std::memory_order_relaxed);
        if ((ac == a && a->m_component.compare_exchange_strong(a, b)) ||
            (b == ac))
          break;
        a = (a->m_component.load(std::memory_order_relaxed))
                ->m_component.load(std::memory_order_relaxed);
        b = b->m_component.load(std::memory_order_relaxed);
      }
    }
  };

  using Graph =
      galois::graphs::LC_CSR_Graph<NodeData,
                                   void>::with_no_lockable<true>::type;
  using GNode          = Graph::GraphNode;
  using component_type = NodeData::component_type;

  void reset(Graph& graph) {
    galois::do_all(
        galois::iterate(graph),
        [&](const GNode& src) {
          graph.getData(src, galois::MethodFlag::UNPROTECTED).reset();
        },
        galois::no_stats(), galois::steal());
  }

  unsigned operator()(Graph& graph) {
    for (uint32_t r = 0; r < NEIGHBOR_SAMPLES; ++r) {
      galois::do_all(
          galois::iterate(graph),
          [&](const GNode& src) {
            Graph::edge_iterator ii =
                graph.edge_begin(src, galois::MethodFlag::UNPROTECTED);
            Graph::edge_iterator ei =
                graph.edge_end(src, galois::MethodFlag::UNPROTECTED);
            for (std::advance(ii, r); ii < ei; ii++) {
              GNode dst = graph.getEdgeDst(ii);
              NodeData& sdata =
                  graph.getData(src, galois::MethodFlag::UNPROTECTED);
              NodeData& ddata =
                  graph.getData(dst, galois::MethodFlag::UNPROTECTED);
              sdata.link(&ddata);
              break;
            }
          },
          galois::steal(), galois::loopname("Afforest-VNS-Link"));

      galois::do_all(
          galois::iterate(graph),
          [&](const GNode& src) {
            NodeData& sdata =
                graph.getData(src, galois::MethodFlag::UNPROTECTED);
            sdata.compress();
          },
          galois::steal(), galois::loopname("Afforest-VNS-Compress"));
    }

    const component_type c = approxLargestComponent<component_type>(graph);

    galois::do_all(
        galois::iterate(graph),
        [&](const GNode& src) {
          NodeData& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
          if (sdata.component() == c)
            return;
          Graph::edge_iterator ii =
              graph.edge_begin(src, galois::MethodFlag::UNPROTECTED);
          Graph::edge_iterator ei =
              graph.edge_end(src, galois::MethodFlag::UNPROTECTED);
          for (std::advance(ii, NEIGHBOR_SAMPLES.getValue()); ii < ei; ++ii) {
            GNode dst = graph.getEdgeDst(ii);
            NodeData& ddata =
                graph.getData(dst, galois::MethodFlag::UNPROTECTED);
            sdata.link(&ddata);
          }
        },
        galois::steal(), galois::loopname("Afforest-LCS-Link"));

    galois::do_all(
        galois::iterate(graph),
        [&](const GNode& src) {
          NodeData& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
          sdata.compress();
        },
        galois::steal(), galois::loopname("Afforest-LCS-Compress"));
    return 0;
  }

  uint64_t components(Graph& graph) {
    galois::GAccumulator<uint64_t> count;
    galois::do_all(
        galois::iterate(graph),
        [&](const GNode& src) {
          if (graph.getData(src, galois::MethodFlag::UNPROTECTED).isRep()) {
            count += 1;
          }
        },
        galois::no_stats());
    return count.reduce();
  }
};

template <typename AlgoT>
void run() {
  using Graph = typename AlgoT::Graph;

  auto seconds = [](std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
  };

  AlgoT impl;
  Graph graph;

  auto start = std::chrono::steady_clock::now();
  galois::graphs::readGraph(graph, inputFile);
  galois::gPrint("load_s ", seconds(start), "\n");

  galois::preAlloc(numThreads +
                   (3 * graph.size() * sizeof(typename Graph::node_data_type)) /
                       galois::runtime::pagePoolSize());

  for (int r = 0; r <= numRuns; ++r) {
    impl.reset(graph);

    start               = std::chrono::steady_clock::now();
    unsigned iterations = impl(graph);
    if (r > 0) {
      galois::gPrint("round ", r - 1, " ", seconds(start), "\n");
      if (iterations > 0) {
        galois::gPrint("iterations ", iterations, "\n");
      }
    }
  }

  galois::gPrint("components ", impl.components(graph), "\n");
}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, nullptr, &inputFile);

  if (!symmetricGraph) {
    GALOIS_DIE("This application requires a symmetric graph input;"
               " please use the -symmetricGraph flag.");
  }

  switch (algo) {
  case Algo::labelProp:
    run<LabelPropAlgo>();
    break;
  case Algo::afforest:
    run<AfforestAlgo>();
    break;
  default:
    std::cerr << "Unknown algorithm\n";
    std::abort();
  }

  return 0;
}
