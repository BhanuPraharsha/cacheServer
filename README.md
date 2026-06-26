# High-Performance C++ In-Memory Cache Server

A high-throughput, low-latency in-memory key-value store written in C++ for Linux. The networking and hashing layers are built from scratch — no Boost.Asio, no libuv, just raw socket syscalls and a custom intrusive hash table.

## Architecture & Features

> [!NOTE]
> For detailed architectural diagrams and reactor flowcharts, please refer to the [Architecture Document](docs/architecture_document.pdf) (PDF) or the [Markdown Version](docs/architecture_document.md).

*   **Networking:** Utilizes native Linux socket APIs without external frameworks.
*   **Event-Driven Reactor:** Uses level-triggered `epoll` for efficient I/O multiplexing and concurrent connection handling.
*   **Non-blocking I/O:**  A state machine handles EAGAIN/EWOULDBLOCK, partial reads/writes, and backpressure.
*   **Progressive Rehashing:** Uses a custom intrusive hash table with separate chaining and the FNV-1a hash algorithm. Rehashing is performed incrementally across two internal tables (`older` and `newer`) to minimize latency spikes on resize.
*   **Binary Protocol:** length-prefixed binary framing protocol to keep parsing overhead low.

### Protocol Specification

The server uses a length-prefixed binary framing protocol to simplify message parsing and enable zero-copy memory mapping.

**Packet Byte Layout:**
| Offset | Size (Bytes) | Field Name | Description |
| :--- | :--- | :--- | :--- |
| `0x00` | 4 | `num_arguments` | Total number of arguments in the command (e.g., 3 for `SET key val`) |
| `0x04` | 4 | `arg1_length` | Length of the first argument (`N`) |
| `0x08` | `N` | `arg1_data` | Raw bytes of the first argument |
| `0x08 + N` | 4 | `arg2_length` | Length of the second argument (`M`) |
| `0x0C + N` | `M` | `arg2_data` | Raw bytes of the second argument |
| ... | ... | ... | *Repeats for all arguments* |

*Example: `GET mykey` translates to:* `[0x00000002] [0x00000003][GET] [0x00000005][mykey]`

##  Performance Benchmarks

Benchmarks were performed on an Intel Core Ultra 7 255HX (20 cores) using `redis-benchmark` over the loopback interface (`127.0.0.1`). The server is single-threaded.

### 1. Pipelined vs. Non-Pipelined Workloads

Pipelining (`-P 16` in `redis-benchmark`) batches multiple commands per request, reducing network round-trip time (RTT) and demonstrating the throughput of the internal event loop and hash table.

**Non-Pipelined (Standard Load: 10M requests, 50 clients, 3-byte payload)**
| Command | Throughput (RPS) | p50 Latency | p99 Latency |
| :--- | :--- | :--- | :--- |
| **SET** | 444,089 req/s | 0.11 ms | 0.22 ms |
| **GET** | 439,212 req/s | 0.11 ms | 0.22 ms |
| **DEL** | 449,074 req/s | 0.10 ms | 0.22 ms |

**Pipelined (Max Throughput: 100M requests, 50 clients, Pipeline 16)**
| Command | Throughput (RPS) | p50 Latency | p99 Latency |
| :--- | :--- | :--- | :--- |
| **SET** | 4,251,339 req/s | 0.18 ms | 0.36 ms |
| **GET** | 4,204,684 req/s | 0.18 ms | 0.36 ms |
| **DEL** | 5,265,275 req/s | 0.14 ms | 0.35 ms |

With the network round-trip mostly out of the way, the core engine pushes past 5.2M ops/sec on a single thread.

### 2. `epoll` vs. `poll`
 
To see whether the reactor choice actually matters in practice, the same server was benchmarked with a `poll()`-based event loop instead of `epoll`, under identical load.

**Standard Load Comparison (10M requests, 50 clients, 3-byte payload)**

| Metric | `poll` Implementation | `epoll` Implementation | Improvement |
| :--- | :--- | :--- | :--- |
| **SET Throughput** | 271,909 req/s | **444,089 req/s** | **+63.3%** |
| **GET Throughput** | 249,956 req/s | **439,212 req/s** | **+75.7%** |
| **DEL Throughput** | 261,226 req/s | **449,074 req/s** | **+71.9%** |

The gap tracks with `epoll`'s O(1) event notification vs. `poll`'s O(N) scan, and it's already visible at fairly modest concurrency.

### 3. Payload Scaling 
This checks how the network buffers (`std::vector<uint8_t>`) and I/O state machine hold up as payloads get bigger.

**1KB Payload (5M requests, 50 clients)**
*   `poll`: 259,551 RPS (p99: 0.37 ms)
*   `epoll`: **425,206 RPS** (p99: 0.22 ms) -> **+63.8%**

**10KB Payload (1M requests, 50 clients)**
*   `poll`: 190,403 RPS (p99: 0.48 ms)
*   `epoll`: **307,219 RPS** (p99: 0.31 ms) -> **+61.3%**

The ~60% gap holds steady across payload sizes, which points to event-handling overhead — not memory bandwidth or buffering — as the real bottleneck here.

### 4. Connection scaling

The server was tested with 10,000 concurrent connections serving 10 million requests.

| Command | Concurrent Clients | Requests | Throughput | p50 Latency |
| :--- | :--- | :--- | :--- | :--- |
| **SET** | 10,000 | 10M | 246,530 req/s | 40.35 ms |
| **GET** | 10,000 | 10M | 249,843 req/s | 39.80 ms |

At 10,000 concurrent connections the server held up without crashing or leaking memory — a reasonable stress test for the non-blocking I/O state machine.

##  Internal Mechanics

The core of the server is a reactor pattern powered by `epoll_wait`. 
 
1. **Accept** — a non-blocking listening socket accepts connections and registers them with `epoll` for `EPOLLIN`.
2. **Read** — incoming bytes go into a per-connection `std::vector<uint8_t>` buffer; once a full message is framed, it's executed immediately.
3. **Write** — responses are written straight to the socket. If the send buffer fills up (`EAGAIN`), the rest is queued and the socket is registered for `EPOLLOUT` until it's writable again.

## Getting Started

### Prerequisites
*   Linux environment 
*   A modern C++ compiler (`g++` or `clang++`) supporting C++17 or higher.
*   `redis-benchmark` (optional, for running performance tests)

### Compilation
Compile with O3 optimizations to maximize performance:
```bash
g++ -O3 -std=c++17 servertest.cpp hashtable.cpp -o cache_server
```

### Running
```bash
sudo chrt -f 99 nice -n -20 ./cache_server
```

