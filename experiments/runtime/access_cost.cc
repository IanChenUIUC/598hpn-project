// Usage: access_cost <compute_us> <runners_per_worker> <accesses_per_worker> <access 1|0>
//
// Measures the core time one remote read costs under load, over YGM request
// and reply (-DUSE_YGM) or a UCX get (-DUSE_UCX). Runs on two MPI ranks with
// two Qthreads workers each. Every runner loops: spin for compute_us, then read
// a random element on the other rank and suspend until the value arrives. Both
// ranks issue and serve at the same time. With access 0 the read is skipped.
//
// Prints one line on rank 0:
//   path P c_us C runners R accesses A seconds S core_ns_per_access N useful_pct U
//   mean_access_us M bad B
// where seconds is the slower rank's time until its runners finish, and N is
// worker count times seconds divided by accesses, over both ranks. bad counts
// wrong values and runners that changed worker. Exits nonzero if bad is not 0.

#include <mpi.h>
#include <qthread/qthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(USE_YGM) == defined(USE_UCX)
#error "define exactly one of USE_YGM and USE_UCX"
#endif

#ifdef USE_YGM
#include <memory>
#include <ygm/comm.hpp>
#else
#include <ucp/api/ucp.h>
#endif

using Clock = std::chrono::steady_clock;

static constexpr int kWorkers = 2;
static constexpr uint64_t kElements = uint64_t(1) << 20;

struct Slot {
  aligned_t full;
  uint64_t value;
};

// Each counter is touched only by the worker it belongs to.
struct alignas(64) Counters {
  uint64_t remaining = 0;
  uint64_t accesses = 0;
  uint64_t wait_ns = 0;
  uint64_t bad = 0;
};

static std::vector<uint64_t> elements;
static Counters counters[kWorkers];
static std::atomic<uint64_t> runners_remaining{0};
static int my_rank;
static int peer_rank;
static std::chrono::nanoseconds compute_time;
static bool do_access;

static uint64_t value_of(int rank, uint64_t index) {
  return (uint64_t(rank + 1) * 0x9E3779B97F4A7C15ULL) ^ (index * 0xC2B2AE3D27D4EB4FULL);
}

#ifdef USE_YGM

static const char* const kPathName = "ygm";
static std::vector<std::unique_ptr<ygm::comm>> comms;
static MPI_Comm duplicated[kWorkers];

void path_init() {
  for (int w = 0; w < kWorkers; w++) {
    MPI_Comm_dup(MPI_COMM_WORLD, &duplicated[w]);
    comms.push_back(std::make_unique<ygm::comm>(duplicated[w]));
  }
}

uint64_t remote_read(int worker, uint64_t index) {
  Slot slot;
  qthread_empty(&slot.full);
  auto on_owner = [](int source, uint64_t i, uintptr_t reply_to) {
    auto on_reply = [](uint64_t value, uintptr_t address) {
      auto* s = reinterpret_cast<Slot*>(address);
      s->value = value;
      qthread_fill(&s->full);
    };
    comms[qthread_shep()]->async(source, on_reply, elements[i], reply_to);
  };
  comms[worker]->async(peer_rank, on_owner, my_rank, index, reinterpret_cast<uintptr_t>(&slot));
  aligned_t ignored;
  qthread_readFF(&ignored, &slot.full);
  return slot.value;
}

void path_poll(int worker) { comms[worker]->local_progress(); }

void path_finish(int worker) { comms[worker]->barrier(); }

void path_shutdown() {
  comms.clear();
  for (MPI_Comm& d : duplicated) MPI_Comm_free(&d);
}

#else

static const char* const kPathName = "ucx";
static ucp_context_h context;
static ucp_mem_h memh;
static void* packed_rkey;
static ucp_worker_h workers[kWorkers];
static ucp_ep_h endpoints[kWorkers];
static ucp_rkey_h rkeys[kWorkers];
static uint64_t remote_base;

void check(ucs_status_t status, const char* what) {
  if (status == UCS_OK) return;
  std::fprintf(stderr, "%s failed: %s\n", what, ucs_status_string(status));
  MPI_Abort(MPI_COMM_WORLD, 1);
}

void progress_all() {
  for (ucp_worker_h w : workers) ucp_worker_progress(w);
}

void progress_through_barrier() {
  MPI_Request barrier;
  MPI_Ibarrier(MPI_COMM_WORLD, &barrier);
  int done = 0;
  while (!done) {
    progress_all();
    MPI_Test(&barrier, &done, MPI_STATUS_IGNORE);
  }
}

// Progresses every worker, since one request's completion can depend on the
// peer progressing a different worker.
void wait_all(ucs_status_ptr_t (&pending)[kWorkers]) {
  bool busy = true;
  while (busy) {
    busy = false;
    progress_all();
    for (ucs_status_ptr_t& request : pending) {
      if (request == nullptr) continue;
      if (UCS_PTR_IS_ERR(request)) check(UCS_PTR_STATUS(request), "operation");
      ucs_status_t status = ucp_request_check_status(request);
      if (status == UCS_INPROGRESS) {
        busy = true;
        continue;
      }
      ucp_request_free(request);
      check(status, "operation");
      request = nullptr;
    }
  }
}

std::vector<char> swap_with_peer(const void* data, uint64_t size) {
  uint64_t peer_size;
  MPI_Sendrecv(&size, 1, MPI_UINT64_T, peer_rank, 0, &peer_size, 1, MPI_UINT64_T, peer_rank, 0,
               MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  std::vector<char> received(peer_size);
  MPI_Sendrecv(data, static_cast<int>(size), MPI_BYTE, peer_rank, 1, received.data(),
               static_cast<int>(peer_size), MPI_BYTE, peer_rank, 1, MPI_COMM_WORLD,
               MPI_STATUS_IGNORE);
  return received;
}

void path_init() {
  ucp_params_t params{};
  params.field_mask = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_MT_WORKERS_SHARED;
  params.features = UCP_FEATURE_RMA;
  params.mt_workers_shared = 1;
  check(ucp_init(&params, nullptr, &context), "ucp_init");

  ucp_mem_map_params_t map{};
  map.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
  map.address = elements.data();
  map.length = elements.size() * sizeof(uint64_t);
  check(ucp_mem_map(context, &map, &memh), "ucp_mem_map");
  size_t rkey_size;
  check(ucp_rkey_pack(context, memh, &packed_rkey, &rkey_size), "ucp_rkey_pack");
  std::vector<char> peer_rkey = swap_with_peer(packed_rkey, rkey_size);

  uint64_t base = reinterpret_cast<uintptr_t>(elements.data());
  MPI_Sendrecv(&base, 1, MPI_UINT64_T, peer_rank, 2, &remote_base, 1, MPI_UINT64_T, peer_rank, 2,
               MPI_COMM_WORLD, MPI_STATUS_IGNORE);

  for (int w = 0; w < kWorkers; w++) {
    ucp_worker_params_t worker_params{};
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;
    check(ucp_worker_create(context, &worker_params, &workers[w]), "ucp_worker_create");

    ucp_address_t* address;
    size_t address_size;
    check(ucp_worker_get_address(workers[w], &address, &address_size), "ucp_worker_get_address");
    std::vector<char> peer_address = swap_with_peer(address, address_size);
    ucp_worker_release_address(workers[w], address);

    ucp_ep_params_t ep_params{};
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address = reinterpret_cast<ucp_address_t*>(peer_address.data());
    check(ucp_ep_create(workers[w], &ep_params, &endpoints[w]), "ucp_ep_create");
    check(ucp_ep_rkey_unpack(endpoints[w], peer_rkey.data(), &rkeys[w]), "ucp_ep_rkey_unpack");
  }

  // Connections are set up here so that the timed run does not include them.
  uint64_t first[kWorkers];
  ucs_status_ptr_t pending[kWorkers];
  for (int w = 0; w < kWorkers; w++) {
    ucp_request_param_t param{};
    pending[w] = ucp_get_nbx(endpoints[w], &first[w], sizeof(uint64_t), remote_base, rkeys[w], &param);
  }
  wait_all(pending);
  progress_through_barrier();
  for (uint64_t value : first) {
    if (value != value_of(peer_rank, 0)) {
      std::fprintf(stderr, "first get returned a wrong value\n");
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
  }
}

void get_done(void* request, ucs_status_t status, void* user_data) {
  check(status, "ucp_get_nbx");
  qthread_fill(&static_cast<Slot*>(user_data)->full);
  ucp_request_free(request);
}

uint64_t remote_read(int worker, uint64_t index) {
  Slot slot;
  qthread_empty(&slot.full);
  ucp_request_param_t param{};
  param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
  param.cb.send = get_done;
  param.user_data = &slot;
  ucs_status_ptr_t request =
      ucp_get_nbx(endpoints[worker], &slot.value, sizeof(uint64_t),
                  remote_base + index * sizeof(uint64_t), rkeys[worker], &param);
  if (request == nullptr) return slot.value;
  if (UCS_PTR_IS_ERR(request)) check(UCS_PTR_STATUS(request), "ucp_get_nbx");
  aligned_t ignored;
  qthread_readFF(&ignored, &slot.full);
  return slot.value;
}

void path_poll(int worker) { ucp_worker_progress(workers[worker]); }

void path_finish(int) {}

void path_shutdown() {
  progress_through_barrier();
  ucs_status_ptr_t pending[kWorkers];
  for (int w = 0; w < kWorkers; w++) {
    ucp_rkey_destroy(rkeys[w]);
    ucp_request_param_t param{};
    pending[w] = ucp_ep_close_nbx(endpoints[w], &param);
  }
  wait_all(pending);
  progress_through_barrier();
  ucp_rkey_buffer_release(packed_rkey);
  check(ucp_mem_unmap(context, memh), "ucp_mem_unmap");
  for (ucp_worker_h w : workers) ucp_worker_destroy(w);
  ucp_cleanup(context);
}

#endif

static void spin() {
  if (compute_time.count() == 0) return;
  auto start = Clock::now();
  while (Clock::now() - start < compute_time) {
  }
}

aligned_t runner_task(void* arg) {
  int w = qthread_shep();
  Counters& c = counters[w];
  uint64_t rng = (uint64_t(my_rank + 1) << 40) ^ (reinterpret_cast<uintptr_t>(arg) << 8) ^ 0x2545F4914F6CDD1DULL;
  while (c.remaining > 0) {
    --c.remaining;
    spin();
    if (do_access) {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      uint64_t index = rng % kElements;
      auto start = Clock::now();
      uint64_t value = remote_read(w, index);
      c.wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
      if (value != value_of(peer_rank, index) || qthread_shep() != w) ++c.bad;
    }
    ++c.accesses;
  }
  runners_remaining.fetch_sub(1);
  return 0;
}

aligned_t poller_task(void* arg) {
  int w = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
  while (runners_remaining.load(std::memory_order_relaxed) > 0) {
    path_poll(w);
    qthread_yield();
  }
  path_finish(w);
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr,
                 "usage: %s <compute_us> <runners_per_worker> <accesses_per_worker> <access 1|0>\n",
                 argv[0]);
    return 2;
  }
  double compute_us = std::atof(argv[1]);
  uint64_t runners_per_worker = std::strtoull(argv[2], nullptr, 10);
  uint64_t accesses_per_worker = std::strtoull(argv[3], nullptr, 10);
  do_access = std::atoi(argv[4]) != 0;
  compute_time = std::chrono::nanoseconds(static_cast<int64_t>(compute_us * 1000));

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

  elements.resize(kElements);
  for (uint64_t i = 0; i < kElements; i++) elements[i] = value_of(my_rank, i);

  path_init();

  if (qthread_initialize() != 0) {
    std::fprintf(stderr, "qthread_initialize failed\n");
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  if (qthread_num_shepherds() != kWorkers || qthread_num_workers() != kWorkers) {
    std::fprintf(stderr, "expected %d shepherds and workers, got %d and %d\n", kWorkers,
                 int(qthread_num_shepherds()), int(qthread_num_workers()));
    MPI_Abort(MPI_COMM_WORLD, 1);
  }

  for (Counters& c : counters) c.remaining = accesses_per_worker;
  runners_remaining.store(runners_per_worker * kWorkers);
  std::vector<aligned_t> runner_done(runners_per_worker * kWorkers);
  aligned_t poller_done[kWorkers];

  MPI_Barrier(MPI_COMM_WORLD);
  auto start = Clock::now();
  for (uintptr_t w = 0; w < kWorkers; w++) {
    qthread_fork_to(poller_task, reinterpret_cast<void*>(w), &poller_done[w], w);
  }
  for (uint64_t r = 0; r < runner_done.size(); r++) {
    qthread_fork_to(runner_task, reinterpret_cast<void*>(uintptr_t(r + 1)), &runner_done[r],
                    r % kWorkers);
  }

  aligned_t ignored;
  for (aligned_t& done : runner_done) qthread_readFF(&ignored, &done);
  double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  for (aligned_t& done : poller_done) qthread_readFF(&ignored, &done);
  qthread_finalize();

  path_shutdown();

  uint64_t local[3] = {0, 0, 0};
  for (const Counters& c : counters) {
    local[0] += c.accesses;
    local[1] += c.wait_ns;
    local[2] += c.bad;
  }
  uint64_t total[3];
  double slowest;
  MPI_Allreduce(local, total, 3, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&seconds, &slowest, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

  if (my_rank == 0) {
    double core_ns = kWorkers * nranks * slowest * 1e9 / double(total[0]);
    std::printf(
        "path %s c_us %.3f runners %llu accesses %llu seconds %.6f core_ns_per_access %.1f "
        "useful_pct %.1f mean_access_us %.3f bad %llu\n",
        kPathName, compute_us, (unsigned long long)runners_per_worker,
        (unsigned long long)total[0], slowest, core_ns, 100.0 * compute_us * 1000 / core_ns,
        double(total[1]) / double(total[0]) / 1000.0, (unsigned long long)total[2]);
    std::fflush(stdout);
  }

  MPI_Finalize();
  return total[2] == 0 ? 0 : 1;
}
