// Weakly connected components with libgrape-lite's
// examples/analytical_apps/wcc, loaded as run_app loads it for "wcc"
// (undirected, map partitioner, no rebalancing), printing on worker 0:
//   load_s <seconds>         reading and partitioning
//   round <r> <seconds>      one line per timed round, after one untimed warm-up
//   components <count>
//
// Usage: mpirun -n P cc [--rounds R] [--threads T] <prefix>
// (reads prefix.v and prefix.e)

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include <gflags/gflags.h>
#include <grape/fragment/immutable_edgecut_fragment.h>
#include <grape/fragment/loader.h>
#include <grape/grape.h>

#include "wcc/wcc.h"

DEFINE_int32(rounds, 3, "timed rounds after the warm-up");
DEFINE_int32(threads, 1, "threads per process");

using FRAG_T =
    grape::ImmutableEdgecutFragment<int64_t, uint32_t, grape::EmptyType,
                                    grape::EmptyType,
                                    grape::LoadStrategy::kOnlyOut>;
using APP_T = grape::WCC<FRAG_T>;

static double seconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                       start)
      .count();
}

int main(int argc, char* argv[]) {
  grape::gflags::SetUsageMessage("mpirun -n P cc [--rounds R] [--threads T] <prefix>");
  grape::gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (argc != 2) {
    grape::gflags::ShowUsageWithFlagsRestrict(argv[0], "cc.cc");
    return 1;
  }
  std::string prefix = argv[1];

  grape::InitMPIComm();
  {
    grape::CommSpec comm_spec;
    comm_spec.Init(MPI_COMM_WORLD);
    bool coordinator = comm_spec.worker_id() == grape::kCoordinatorRank;

    grape::LoadGraphSpec graph_spec = grape::DefaultLoadGraphSpec();
    graph_spec.set_directed(false);
    graph_spec.set_rebalance(false, 0);
    graph_spec.partitioner_type = grape::parse_partitioner_type_name("map");
    graph_spec.idxer_type = grape::parse_idxer_type_name("hashmap");

    MPI_Barrier(comm_spec.comm());
    auto start = std::chrono::steady_clock::now();
    auto fragment = grape::LoadGraph<FRAG_T>(prefix + ".e", prefix + ".v",
                                             comm_spec, graph_spec);
    MPI_Barrier(comm_spec.comm());
    if (coordinator) {
      std::cout << "load_s " << seconds(start) << std::endl;
    }

    auto app = std::make_shared<APP_T>();
    auto spec = grape::MultiProcessSpec(comm_spec, false);
    spec.thread_num = FLAGS_threads;
    decltype(APP_T::CreateWorker(app, fragment)) worker;

    for (int run = 0; run <= FLAGS_rounds; ++run) {
      if (worker) {
        worker->Finalize();
      }
      worker = APP_T::CreateWorker(app, fragment);
      worker->Init(comm_spec, spec);

      MPI_Barrier(comm_spec.comm());
      start = std::chrono::steady_clock::now();
      worker->Query();
      MPI_Barrier(comm_spec.comm());
      if (run > 0 && coordinator) {
        std::cout << "round " << run - 1 << " " << seconds(start) << std::endl;
      }
    }

    auto& comp_id = worker->GetContext()->comp_id;
    uint64_t local = 0, total = 0;
    for (auto v : fragment->InnerVertices()) {
      local += comp_id[v] == fragment->GetId(v);
    }
    MPI_Allreduce(&local, &total, 1, MPI_UINT64_T, MPI_SUM, comm_spec.comm());
    if (coordinator) {
      std::cout << "components " << total << std::endl;
    }

    worker->Finalize();
  }
  grape::FinalizeMPIComm();
}
