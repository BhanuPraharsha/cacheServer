# High-Performance C++ In-Memory Cache Server

A high-throughput, low-latency in-memory key-value store built entirely from scratch in C++ for Linux. Designed to maximize single-thread performance, this project implements a custom intrusive hash table and a highly optimized TCP network stack directly on top of Linux system calls, bypassing high-level abstractions to work directly with the kernel.

## 🚀 System Architecture & Key Features

> [!NOTE]
> For a full architectural deep-dive, including sequence diagrams and reactor flowcharts, please refer to the [Architecture Document](docs/architecture_document.pdf) (PDF) or the [Markdown Version](docs/architecture_document.md).

*   **Bare-Metal Networking:** Built without external dependencies or heavy frameworks (no Boost.Asio, no libuv). Relies purely on Linux socket APIs.
*   **Event-Driven Reactor Pattern:** Utilizes level-triggered `epoll` to handle massive concurrency (the C10K problem) efficiently, achieving O(1) event notification complexity.
*   **Zero-Copy & Stateful I/O:** Implements a custom state machine to handle non-blocking I/O (`EAGAIN`/`EWOULDBLOCK`), partial reads/writes, and kernel buffer backpressure gracefully without blocking the event loop.
*   **Progressive Rehashing:** Implements a custom intrusive hash table utilizing separate chaining. To prevent catastrophic latency spikes during dynamic resizing, the table performs incremental bucket migrations across two internal tables (`older` and `newer`) over successive operations. Uses the FNV-1a string hashing algorithm.
*   **Custom Binary Protocol:** Uses a lightweight, length-prefixed binary framing protocol to eliminate parsing overhead inherent in text-based protocols (like RESP).

### Protocol Specification
To eliminate the CPU overhead of parsing text strings (like Redis's RESP), this server uses a custom length-prefixed binary framing protocol. This allows the server to parse commands instantly using simple pointer arithmetic and zero-copy memory mapping.

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

## 📊 Performance Benchmarks

All benchmarks were executed on an Intel Core Ultra 7 255HX (20 cores) using `redis-benchmark` over the loopback interface (`127.0.0.1`). Note that the server operates on a **single thread**, maximizing single-core utilization to achieve these metrics.

### 1. Pipelined vs. Non-Pipelined Workloads (Network vs CPU Bottleneck)
In a standard client-server model, throughput is severely bottlenecked by network round-trip time (RTT). When a client sends one command and waits for a response (non-pipelined), the server's CPU spends most of its time idle waiting for kernel network buffers to flush.

By utilizing **pipelining** (`-P 16` in `redis-benchmark`), the client sends batches of 16 commands at once. This bypasses the network RTT bottleneck, drastically reducing `read()`/`write()` syscalls and revealing the raw, true execution speed of the internal C++ event loop and hash table implementation.

**Non-Pipelined (Standard Load: 10M requests, 50 clients, 3-byte payload)**
| Command | Throughput (RPS) | p50 Latency | p99 Latency |
| :--- | :--- | :--- | :--- |
| **SET** | 444,089 req/s | 0.11 ms | 0.22 ms |
| **GET** | 439,212 req/s | 0.11 ms | 0.22 ms |
| **DEL** | 449,074 req/s | 0.10 ms | 0.22 ms |

**Pipelined (Max Throughput: 100M requests, 50 clients, Pipeline 16)**
| Command | Throughput (RPS) | p50 Latency | p99 Latency |
| :--- | :--- | :--- | :--- |
| **SET** | **4,251,339 req/s** | 0.18 ms | 0.36 ms |
| **GET** | **4,204,684 req/s** | 0.18 ms | 0.36 ms |
| **DEL** | **5,265,275 req/s** | 0.14 ms | 0.35 ms |

*Conclusion:* The core engine can execute over **5.2 million operations per second** on a single thread when not bounded by TCP network round-trips.

### 2. Architectural Comparison: `epoll` vs `poll`
To empirically demonstrate the scaling limitations of older system calls, an alternative implementation using `poll()` was built and benchmarked against the main `epoll` architecture under standard non-pipelined loads.

**Standard Load Comparison (10M requests, 50 clients, 3-byte payload)**

| Metric | `poll` Implementation | `epoll` Implementation | Improvement |
| :--- | :--- | :--- | :--- |
| **SET Throughput** | 271,909 req/s | **444,089 req/s** | **+63.3%** |
| **GET Throughput** | 249,956 req/s | **439,212 req/s** | **+75.7%** |
| **DEL Throughput** | 261,226 req/s | **449,074 req/s** | **+71.9%** |

*Conclusion:* The O(1) event notification complexity of `epoll` drastically outperforms the O(N) complexity of `poll()`, yielding massive performance gains even under standard concurrency.

### 3. Payload Scaling Impact
Testing how the custom network buffers (`std::vector<uint8_t>`) and stateful I/O handle larger memory copies over the socket.

**1KB Payload (5M requests, 50 clients)**
*   `poll`: 259,551 RPS (p99: 0.37 ms)
*   `epoll`: **425,206 RPS** (p99: 0.22 ms) -> **+63.8%**

**10KB Payload (1M requests, 50 clients)**
*   `poll`: 190,403 RPS (p99: 0.48 ms)
*   `epoll`: **307,219 RPS** (p99: 0.31 ms) -> **+61.3%**

*Conclusion:* The `epoll` architecture maintains its ~60% dominant lead across different payload sizes, demonstrating that network event handling overhead is the primary system bottleneck, not memory bandwidth or user-space buffering.

### 4. High Concurrency (The C10K Test)
The true test of the event loop. The server was bombarded with 10,000 concurrent, active connections while serving 10 million requests.

| Command | Concurrent Clients | Requests | Throughput | p50 Latency |
| :--- | :--- | :--- | :--- | :--- |
| **SET** | 10,000 | 10M | 246,530 req/s | 40.35 ms |
| **GET** | 10,000 | 10M | 249,843 req/s | 39.80 ms |

*Conclusion:* The server gracefully managed 10,000 concurrent connections on a single thread without memory exhaustion or process crashing, proving the stability of the non-blocking I/O and `epoll` state machine.

## 🛠️ Internal Mechanics Deep Dive

The core of the server is a reactor pattern powered by `epoll_wait`. 

1. **Acceptor:** A non-blocking listening socket accepts incoming TCP connections and registers them with the `epoll` instance with `EPOLLIN` interest.
2. **Fast-Path Reading:** When data arrives, the server reads into a pre-allocated per-connection `std::vector<uint8_t>` buffer. If a complete message is framed according to the custom protocol, it is immediately executed, bypassing unnecessary event loop iterations.
3. **Stateful Writing:** Responses are written directly to the socket. If the kernel's TCP send buffer fills up (`EAGAIN`), the remaining data is buffered in user-space, and the socket state is mutated to watch for `EPOLLOUT`. The event loop automatically resumes sending when the socket becomes writable.

## 🚀 Getting Started

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

## 🔮 Future Roadmap

- **Multi-threading:** Transitioning from a single-threaded reactor to a multi-threaded architecture.
- **Eviction Policies (LRU/LFU):** Implementing a doubly-linked list mechanism over the hash table to enforce memory limits.
- **Time-To-Live (TTL):** Implementing active and passive expiration of keys.
- **Persistence:** background snapshotting to disk.
