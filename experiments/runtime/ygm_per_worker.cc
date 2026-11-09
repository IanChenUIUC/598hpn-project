// Usage: ygm_per_worker <tasks_per_worker> <requests_per_task> <poll 1|0> <skew 1|0>
//
// With skew 1 all clients start on worker 0, so other workers only get clients
// by stealing them.
//
// Checks that one ygm::comm per Qthreads worker can carry request and reply
// traffic for tasks that suspend until their reply arrives. Runs on two MPI
// ranks. Worker i uses comm i, and comm i pairs with comm i on the other rank.
// With poll 0 the workers never call local_progress, so replies cannot arrive
// and the run is expected to time out.
//
// Prints one line per rank and worker, then on rank 0 the line "pass 1" or
// "pass 0":
//   rank R worker W sent S handled H replied P wrong_source X migrated M

#include <mpi.h>
#include <qthread/qthread.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <ygm/comm.hpp>

static constexpr int kWorkers = 2;

struct Slot {
  aligned_t full;
  uint64_t value;
};

// Each counter is touched only by the worker it belongs to.
struct alignas(64) Counters {
  uint64_t sent = 0;
  uint64_t handled = 0;
  uint64_t replied = 0;
  uint64_t wrong_source = 0;
  uint64_t migrated = 0;
  uint64_t bad_value = 0;
};

static std::vector<std::unique_ptr<ygm::comm>> comms;
static Counters counters[kWorkers];
static std::atomic<uint64_t> clients_remaining{0};
static int my_rank;
static int peer_rank;
static uint64_t requests_per_task;
static bool poll_enabled;

void request(ygm::comm& c, int dest, uint64_t value, Slot* slot) {
  auto on_owner = [](int source, uint64_t v, uintptr_t reply_to) {
    int w = qthread_shep();
    ++counters[w].handled;
    if (source == my_rank) ++counters[w].wrong_source;
    auto on_reply = [](uint64_t result, uintptr_t address) {
      ++counters[qthread_shep()].replied;
      auto* s = reinterpret_cast<Slot*>(address);
      s->value = result;
      qthread_fill(&s->full);
    };
    comms[w]->async(source, on_reply, v + 1, reply_to);
  };
  c.async(dest, on_owner, my_rank, value, reinterpret_cast<uintptr_t>(slot));
}

aligned_t client_task(void* arg) {
  uint64_t id = reinterpret_cast<uintptr_t>(arg);
  Slot slot;
  for (uint64_t k = 0; k < requests_per_task; k++) {
    int w = qthread_shep();
    uint64_t value = (id << 32) | k;
    qthread_empty(&slot.full);
    ++counters[w].sent;
    request(*comms[w], peer_rank, value, &slot);
    aligned_t ignored;
    qthread_readFF(&ignored, &slot.full);
    int resumed = qthread_shep();
    if (slot.value != value + 1) ++counters[resumed].bad_value;
    if (resumed != w) ++counters[resumed].migrated;
  }
  clients_remaining.fetch_sub(1);
  return 0;
}

aligned_t poller_task(void* arg) {
  ygm::comm& c = *comms[reinterpret_cast<uintptr_t>(arg)];
  while (clients_remaining.load() > 0) {
    if (poll_enabled) c.local_progress();
    qthread_yield();
  }
  c.barrier();
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: %s <tasks_per_worker> <requests_per_task> <poll 1|0> <skew 1|0>\n",
                 argv[0]);
    return 2;
  }
  bool skew = std::atoi(argv[4]) != 0;
  uint64_t tasks_per_worker = std::strtoull(argv[1], nullptr, 10);
  requests_per_task = std::strtoull(argv[2], nullptr, 10);
  poll_enabled = std::atoi(argv[3]) != 0;

  int provided;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
  if (provided < MPI_THREAD_MULTIPLE) {
    std::fprintf(stderr, "MPI_THREAD_MULTIPLE not provided: %d\n", provided);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  int nranks;
  MPI_Comm_size(MPI_COMM_WORLD, &nranks);
  MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
  if (nranks != 2) {
    std::fprintf(stderr, "expected 2 ranks, got %d\n", nranks);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  peer_rank = 1 - my_rank;

  MPI_Comm duplicated[kWorkers];
  for (int i = 0; i < kWorkers; i++) {
    MPI_Comm_dup(MPI_COMM_WORLD, &duplicated[i]);
    comms.push_back(std::make_unique<ygm::comm>(duplicated[i]));
  }

  if (qthread_initialize() != 0) {
    std::fprintf(stderr, "qthread_initialize failed\n");
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  if (qthread_num_shepherds() != kWorkers || qthread_num_workers() != kWorkers) {
    std::fprintf(stderr, "expected %d shepherds and workers, got %d and %d\n", kWorkers,
                 int(qthread_num_shepherds()), int(qthread_num_workers()));
    MPI_Abort(MPI_COMM_WORLD, 1);
  }

  clients_remaining.store(tasks_per_worker * kWorkers);
  std::vector<aligned_t> client_done(tasks_per_worker * kWorkers);
  aligned_t poller_done[kWorkers];
  for (uintptr_t w = 0; w < kWorkers; w++) {
    qthread_fork_to(poller_task, reinterpret_cast<void*>(w), &poller_done[w], w);
  }
  for (uint64_t t = 0; t < client_done.size(); t++) {
    qthread_fork_to(client_task, reinterpret_cast<void*>(uintptr_t(t + 1)), &client_done[t],
                    skew ? 0 : t % kWorkers);
  }

  aligned_t ignored;
  for (aligned_t& done : client_done) qthread_readFF(&ignored, &done);
  for (aligned_t& done : poller_done) qthread_readFF(&ignored, &done);
  qthread_finalize();

  comms.clear();
  for (MPI_Comm& d : duplicated) MPI_Comm_free(&d);

  Counters theirs[kWorkers];
  MPI_Sendrecv(counters, sizeof(counters), MPI_BYTE, peer_rank, 0, theirs, sizeof(theirs), MPI_BYTE,
               peer_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

  bool pass = true;
  for (int w = 0; w < kWorkers; w++) {
    const Counters& c = counters[w];
    pass = pass && c.sent > 0 && c.handled == theirs[w].sent && c.replied == c.sent &&
           c.wrong_source == 0 && c.bad_value == 0;
    std::printf("rank %d worker %d sent %llu handled %llu replied %llu wrong_source %llu migrated %llu\n",
                my_rank, w, (unsigned long long)c.sent, (unsigned long long)c.handled,
                (unsigned long long)c.replied, (unsigned long long)c.wrong_source,
                (unsigned long long)c.migrated);
  }
  std::fflush(stdout);

  int local_pass = pass ? 1 : 0;
  int all_pass;
  MPI_Allreduce(&local_pass, &all_pass, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
  if (my_rank == 0) std::printf("pass %d\n", all_pass);

  MPI_Finalize();
  return all_pass ? 0 : 1;
}
