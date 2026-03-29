HTTP Proxy Webserver

### Project Overview

This proxy server acts as a middleman between a client (like a web browser or curl) and the internet. When a client requests a web page, the proxy fetches it on their behalf. If the request is standard HTTP, the proxy saves a copy of the response in its cache so future requests for the same page are served instantly without contacting the remote server. For HTTPS, it creates a secure, encrypted tunnel directly to the destination.

### Core Features

* Multi-threading: Utilizes POSIX threads (pthread) and semaphores to handle multiple concurrent client connections efficiently.
* HTTPS Support: Implements the HTTP CONNECT method utilizing the select() system call to establish a blind TCP relay for encrypted TLS traffic.
* Algorithmic Caching: Features a highly optimized O(1) Least Recently Used (LRU) cache to store and retrieve web responses.
* Thread-Safe Logging: Custom synchronized logging system to track requests, cache states, and network errors.

---

### Repository Structure

The codebase is organized into strict architectural boundaries separating implementation, interfaces, and testing:

* src/
  * server.c: The core proxy logic, connection handling, and cache implementation.
  * proxy_parse.c: Library for parsing raw HTTP headers and request lines.
  * logger.c: Cuistom thread-safe logging implementation.
* include/
  * proxy_parse.h: Declarations for the HTTP parser.
  * logger.h: Declarations for the custom logging system.
* benchmarks/
  * benchmark_cache.cpp: Google Benchmark C++ routines to measure cache latency and throughput.
* Makefile: Automated build configuration for the proxy binary and benchmarking tools.

---

### Code Architecture and Network Flow

* Network Socket: A software endpoint that establishes a bidirectional communication link between two programs operating over a network.
* File Descriptor (FD): An abstract indicator (integer) used by the Linux kernel to access I/O resources, such as files or network sockets.
* POSIX Threads (pthreads): An execution model that allows a program to control multiple independent flows of work (threads) concurrently within the same process memory space.
* Semaphore: A synchronization variable used to control access to a common resource by multiple processes or threads. In this proxy, it limits the maximum number of concurrent client connections.
* Multiplexing (select): A system call that monitors multiple file descriptors to see if they are ready for reading or writing, allowing a single thread to handle multiple I/O streams without blocking indefinitely on a single one.

#### Socket Lifecycle & Data Flow

The execution of the proxy server follows a strict, multi-phased pipeline to manage connections, parse data, and route traffic.

Phase 1: Server Initialization (The Listening Socket)
1. Creation: The main() function calls socket(AF_INET, SOCK_STREAM, 0) to request a new IPv4 TCP socket from the kernel. This is the primary server socket (socketID).
2. Configuration: setsockopt() is applied with SO_REUSEADDR to prevent the "Address already in use" error if the proxy is restarted rapidly.
3. Binding & Listening: The socket is mapped to INADDR_ANY and the specified PORT using bind(). The server then enters a passive listening state via listen(), maintaining a queue of incoming client connection requests.

Phase 2: Client Acceptance & Concurrency
1. Acceptance: The main thread enters an infinite while(1) loop, calling accept(). When a client (e.g., a web browser) connects, the kernel generates a brand new socket specifically for this client (clientSocketID).
2. Concurrency Control: Before processing, the main thread dispatches a new POSIX thread (pthread_create) and passes the clientSocketID to the thread_fn routine.
3. Throttling: Inside thread_fn, a sem_wait(&semaphore) is immediately called. If the number of active threads exceeds MAX_CLIENTS, the thread sleeps until another connection closes, preventing resource exhaustion.

Phase 3: Request Interception & Parsing
1. Ingestion: The thread reads the raw HTTP text from the clientSocketID using recv() until it detects the \r\n\r\n sequence, which denotes the end of the HTTP headers.
2. Method Extraction: The raw buffer is scanned using sscanf() to identify the HTTP Method (e.g., GET, POST, CONNECT) and the target URI.
3. Parser Execution: For standard HTTP traffic, the buffer is passed to ParsedRequest_parse() (from proxy_parse.c), which tokenizes the headers into a dynamically allocated C structure, allowing the proxy to inject or modify headers (like forcing Connection: close).

Phase 4: Routing & Upstream Connection
Depending on the HTTP Method, the proxy splits into two distinct routing paths:

* Path A: HTTPS Traffic (CONNECT Method)
  1. The proxy acts as a blind relay. It extracts the target domain and port directly from the URI.
  2. A new socket (remoteSocketID) is created and connected to the destination server (e.g., www.google.com:443).
  3. The proxy sends a 200 Connection Established message back to the client.
  4. The handle_connect_tunnel() function is invoked. It uses select() to monitor both clientSocketID and remoteSocketID. When encrypted TLS bytes arrive on one socket, they are instantly forwarded to the other via send() and recv(), without any parsing.

* Path B: Standard HTTP Traffic (GET, POST, etc.)
  1. The thread queries the O(1) LRU Hash Map via find().
  2. Cache Hit: If the URL exists, the cached HTML/data is immediately written back to the clientSocketID.
  3. Cache Miss: A remoteSocketID is created and connected to the destination server. The modified HTTP headers and any payload body are sent upstream.
  4. The response from the upstream server is received in chunks, forwarded to the client, and simultaneously buffered in memory.
  5. Once the transfer completes, if the method was GET, the buffered response is inserted into the cache via addCache().

Phase 5: Teardown
1. Socket Closure: Once the transaction or tunnel completes, the thread calls shutdown(socket, SHUT_RDWR) to gracefully terminate TCP transmission, followed by close(socket) to return the file descriptor to the kernel.
2. Resource Release: Dynamically allocated memory buffers are passed to free().
3. Signal Completion: The thread executes sem_post(&semaphore) to increment the semaphore, waking up any pending connections waiting in the queue, and then safely exits.

---

### Cache Architecture Evolution

 A cache replacement policy that discards the least recently used items first. This algorithm requires keeping track of what was used when, which can be computationally expensive if not implemented with optimized data structures.


| Metric / Feature | Previous Architecture | Upgraded Architecture |
| :--- | :--- | :--- |
| Time Complexity | O(N) | O(1) Amortized |
| Lookup Structure | Singly Linked List | Hash Map (djb2 hashing algorithm) |
| Ordering Structure | Implicit (Time-based scan) | Doubly Linked List (DLL) |
| Lookup Mechanism | Linear traversal of all nodes | Direct array index access via hash |
| Eviction Mechanism | Iterate full list to find oldest time_t | Direct removal of the DLL tail pointer |
| CPU Time (Benchmark) | ~14,726 ns | ~6,327 ns (57% Reduction) |

---

### Usage Instructions

#### 1. Compilation
Ensure GCC and Make are installed on the system. Compile the main proxy executable:
make

#### 2. Running the Server
Start the proxy server by specifying a listening port (e.g., 8080):
./proxy 8080

#### 3. Testing the Proxy
USER can verify the proxy operation using curl from a separate terminal.

* Standard HTTP (Cacheable):
  curl -v -x http://localhost:8080 http://httpbin.org/get
* HTTPS Tunneling (Blind Relay):
  curl -v -x http://localhost:8080 https://www.google.com/

#### 4. Benchmarking
To verify the O(1) cache performance, USER must have libbenchmark-dev installed system-wide.
make report

---

### Relevant Links
* HTTP CONNECT Method Specification: https://developer.mozilla.org/en-US/docs/Web/HTTP/Methods/CONNECT
* POSIX select() System Call: https://man7.org/linux/man-pages/man2/select.2.html
* Google Benchmark Documentation: https://github.com/google/benchmark
* HTTPS-PROXY library: https://github.com/sameer2800/HTTP-PROXY/tree/master