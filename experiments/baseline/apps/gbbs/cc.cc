// Connected components with GBBS's ConnectIt: k-out sampling, then union-find
// with Rem's CAS unite, find_compress and split_atomic_one, printing:
//   load_s <seconds>         mapping the binary graph (edges are faulted in
//                            during the warm-up)
//   round <r> <seconds>      one line per timed round, after one untimed warm-up
//   components <count>
//
// Usage: cc [-rounds R] <file.bin>   (GBBS binary format, -DGBBSLONG)

#include <iostream>

#include "benchmarks/Connectivity/ConnectIt/framework.h"
#include "benchmarks/Connectivity/common.h"
#include "benchmarks/Connectivity/ConnectIt/mains/check.h"
#include "gbbs/gbbs.h"

namespace gbbs {

template <class Graph>
sequence<parent> Compute(Graph& G, commandLine& P) {
  return connectit::run_uf_alg<Graph, sample_kout, find_compress,
                               unite_rem_cas, split_atomic_one>(G, P);
}

}  // namespace gbbs

int main(int argc, char* argv[]) {
  gbbs::commandLine P(argc, argv, " [-rounds R] <file.bin>");
  char* iFile = P.getArgument(0);
  long rounds = P.getOptionLongValue("-rounds", 3);

  gbbs::timer t;
  t.start();
  auto G = gbbs::gbbs_io::read_unweighted_symmetric_graph(iFile, true, true);
  std::cout << "load_s " << t.stop() << std::endl;

  auto labels = gbbs::Compute(G, P);
  for (long r = 0; r < rounds; r++) {
    t.start();
    labels = gbbs::Compute(G, P);
    std::cout << "round " << r << " " << t.stop() << std::endl;
  }

  size_t components = gbbs::num_cc(labels);
  std::cout << "components " << components << std::endl;
}
