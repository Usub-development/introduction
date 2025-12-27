# Asynchronous backend on modern C++ 23.

First of all I need to tell that this article is about our C++ 23 stack currently used in production and produced good performance. Mostly all our libraries based on top of three basic libraries:

- [uvent](https://github.com/Usub-development/uvent) – asynchronous cross-platform engine which provides coroutines wrappers (like in [boost.asio](https://www.boost.org/doc/libs/latest/doc/html/boost_asio.html)) and cross-platform I/O for all of our libraries.
- [unet](https://github.com/Usub-development/unet) – asynchronous web-server based on `uvent` which allows to use coroutines as handlers for endpoints (a good advantage, to be explained later).
- [ureflect](https://github.com/Usub-development/ureflect) – compile time reflection (without need to use a preprocessor).

In this article we'll also use some other our libraries:

- [upq](https://github.com/Usub-development/upq) – asynchronous PostgreSQL client library build on top of [libpq](https://www.postgresql.org/docs/current/libpq.html), `uvent`, `ureflect`. Provides ability to query data from data base with or without reflection (JSON/JSONB also can be parsed into datastructures via reflection).
- [uredis](https://github.com/Usub-development/uredis) – asynchronous redis library build on top of `uvent` with own RESP3 implementation.
- [ulog](https://github.com/Usub-development/ulog) – logger inspired by [spdlog](https://github.com/gabime/spdlog) build on top of `uvent`. Instead of using own thread spawns flushing coroutine.
- [ujson](https://github.com/Usub-development/ujson) – simple json library with reflection.

> This article isn't a guide how to get into coroutines however it shows what can be done by using them in correct way.

## Configuring databases
### PostgreSQL
Before we start using libraries we need to startup `PostgreSQL` instance. Here is the `docker-compose.yaml` file:
```yaml
version: "3.9"

services:
  postgres:
    image: postgres:16
    container_name: local-postgres
    restart: unless-stopped
    environment:
      POSTGRES_USER: dev
      POSTGRES_PASSWORD: devpass
      POSTGRES_DB: devdb
    ports:
      - "5432:5432"
    volumes:
      - pgdata_local:/var/lib/postgresql/data

volumes:
  pgdata_local:
```
### KeyDB
We need also to add `KeyDB` as cache. Let's change `docker-compose.yaml` to:
```yaml
version: "3.9"

services:
  postgres:
    image: postgres:16
    container_name: local-postgres
    restart: unless-stopped
    environment:
      POSTGRES_USER: dev
      POSTGRES_PASSWORD: devpass
      POSTGRES_DB: devdb
    ports:
      - "5432:5432"
    volumes:
      - pgdata_local:/var/lib/postgresql/data

  keydb:
    image: eqalpha/keydb:latest
    container_name: local-keydb
    restart: unless-stopped
    command: ["keydb-server", "/etc/keydb/keydb.conf"]
    ports:
      - "6379:6379"
    volumes:
      - keydbdata_local:/data
      - ./keydb.conf:/etc/keydb/keydb.conf:ro

volumes:
  pgdata_local:
  keydbdata_local:
```
`keydb.conf`:
```conf
bind 0.0.0.0
port 6379

protected-mode yes
requirepass devpass

appendonly yes
dir /data
```
## CMake
After setting up our databases we're able to begin configuring our project. Before we start writing code let's setup `CMakeLists.txt` file correctly:
```cmake
cmake_minimum_required(VERSION 3.27)
project(article)

set(CMAKE_CXX_STANDARD 23)
set(UREDIS_BUILD_EXAMPLES OFF)
set(UREDIS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UREDIS_BUILD_STATIC ON  CACHE BOOL "" FORCE)
set(UREDIS_LOGS OFF CACHE BOOL "" FORCE)
set(UPQ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

include(FetchContent)
include_directories(${article_SOURCE_DIR})

FetchContent_Declare(
        uvent
        GIT_REPOSITORY https://github.com/Usub-development/uvent.git
        GIT_TAG main
        OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
        unet
        GIT_REPOSITORY https://github.com/Usub-development/unet.git
        GIT_TAG main
        OVERRIDE_FIND_PACKAGE
)
# Loading ujson from upq 
FetchContent_Declare(
        upq
        GIT_REPOSITORY https://github.com/Usub-development/upq.git
        GIT_TAG main
        OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
        ulog
        GIT_REPOSITORY https://github.com/Usub-development/ulog.git
        GIT_TAG main
        OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
        uredis
        GIT_REPOSITORY https://github.com/Usub-development/uredis.git
        GIT_TAG main
        FIND_PACKAGE_ARGS
)


FetchContent_MakeAvailable(uvent unet ulog upq uredis)

add_executable(${PROJECT_NAME}
        src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
        /usr/local/include
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
        -lpq
        OpenSSL::Crypto
        usub::uvent
        usub::server
        ZLIB::ZLIB
        usub::upq
        usub::ulog
        usub::uredis
)
```
Our project structure should look like:
```
.
├── CMakeLists.txt
├── docker-compose.yaml
├── include
├── keydb.conf
└── src
    └── main.cpp
```
## Creating debug utils
To simplify error logging we need to create file `LoggingUtils.h`:
```cpp
#ifndef LOGGINGUTILS_H
#define LOGGINGUTILS_H

#include <source_location>
#include <string>

inline std::string make_location_string(const std::source_location& loc = std::source_location::current()) {
    using namespace std::string_literals;
    return std::string(loc.file_name()) + "(" +
           std::to_string(loc.line()) + ":" +
           std::to_string(loc.column()) + ") `" +
           loc.function_name() + "`";
}

#endif //LOGGINGUTILS_H
```
And place it to `include/utils/LoggingUtils.h`.

> If you're using CLion as an IDE, add file to `CMakeFiles.txt`:
```cmake
add_executable(${PROJECT_NAME}
src/main.cpp
include/utils/LoggingUtils.h
)
``` 
To allow CLion check it with static analyzer.

When used in logs, `make_location_string` provides the most detailed indication of the call location, which is very useful for detecting errors.

## Creating `main.cpp`
Now we're able to start configuring our `main.cpp`.

### Adding necessary includes
```cpp
#include <server/server.h>
#include <upq/PgRouting.h>
#include <upq/PgRoutingBuilder.h>
#include <ulog/ulog.h>
#include <uredis/RedisClusterClient.h>

#include "utils/LoggingUtils.h"
```
> Don't worry about `RedisClusterClient`, it'll fallback to basic client if KeyDB (same as Redis) not in cluster mode.

### Configuring `ulog`
```cpp
int main() {
    usub::ulog::ULogInit cfg{
        .trace_path = nullptr, // nullptr means stdout
        .debug_path = nullptr, // -//-
        .info_path = nullptr, // -//-
        .warn_path = nullptr, // -//-
        .error_path = nullptr, // -//-
        .critical_path = nullptr, // -//-
        .fatal_path = nullptr, // -//-
        .flush_interval_ns = 5'000'000'000ULL, // 5 seconds
        .queue_capacity = 1024,
        .batch_size = 512,
        .enable_color_stdout = true,
        .json_mode = false,
        .track_metrics = true
    };

    usub::ulog::init(cfg);

    return 0;
}
```

Let's clarify each parameter:
- `trace_path`, `debug_path` etc. – log paths. `nullptr` means logs will be printed to stdout
- `flush_interval_ns` – how often flushing coroutine will be woken up and flush queues.
- `queue_capacity` – capacity of lock-free queue which is used as log storage by default. If capacity exceeded it'll fallback to queue with mutex. You can track fallback metrics like that:

```cpp
if (auto* lg = usub::ulog::Logger::try_instance())
{
    auto overflows = lg->get_overflow_events();
    ulog::info("logger overflows (mpmc full -> mutex fallback) = {}", overflows);
}
```

- `batch_size` – how many elements will be dequeued from storage (both lock-free and non-lock free queue) at once.
- `enable_color_stdout` – responsible for colorful logs.
- `json_mode` – ulog is able to flush logs as json, so they'll look like:

```json
{"time":"2025-10-28 12:03:44.861","thread":3,"level":"I","msg":"starting event loop..."}
```

- `track_metrics` – should ulog track fallback metrics or not.

> `usub::ulog::init(cfg);` initializes the global logger. Call it before any logging; otherwise the program may crash (e.g., segfault).

Before we start creating server instance it's necessary to say that uvent is a public dependency, so you're allowed to use all of it's functionality with coroutines from each of our library or implementing your own logic in coroutines.

### Creating `unet` instance
Let's create toml [config](https://usub-development.github.io/unet/config/) for `unet`:
```toml
[server]
threads = 4
ip_addr = "0.0.0.0"
timeout = 5000

[[listener]]
port = 17000
ssl  = false
```

Now we need to pass path to config to `unet` instance:

```cpp
auto config_path = "../../config.toml";
usub::server::Server server(config_path);
usub::ulog::info("Server configured with TOML at: {}", config_path);
```

## Creating migration coroutine
For migration, I decided to create an example without transactions because I decided to build UPQ use cases sequentially—from simple to more complex. Here is the code:

```cpp
usub::uvent::task::Awaitable<void> migration_coroutine(usub::pg::PgConnector &connector) {
    usub::pg::RouteHint hint{.kind = usub::pg::QueryKind::Write,
                         .consistency = usub::pg::Consistency::Eventual};
    auto pool = connector.route(hint);
    {
        auto r = co_await pool->query_awaitable(R"SQL(
        CREATE TABLE IF NOT EXISTS public.users (
          id BIGSERIAL PRIMARY KEY,
          name TEXT NOT NULL,
          password_hash TEXT NOT NULL,
          roles TEXT[] NOT NULL DEFAULT '{}',
          created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        );
        CREATE UNIQUE INDEX IF NOT EXISTS ux_users_name ON public.users(name);
        )SQL");
        if (!r.ok) {
            usub::ulog::error("PgQuery failed in: {}, code={} | sqlstate='{}' | message='{}'",
                              make_location_string(), toString(r.code),
                              r.err_detail.sqlstate, r.err_detail.message);
            co_return;
        }
    } {
        auto r = co_await pool->query_awaitable(R"SQL(
        CREATE TABLE IF NOT EXISTS public.articles (
          id BIGSERIAL PRIMARY KEY,
          author_id BIGINT NOT NULL REFERENCES public.users(id) ON DELETE RESTRICT,
          title TEXT NOT NULL,
          body TEXT NOT NULL,
          status SMALLINT NOT NULL DEFAULT 0, -- 0=draft, 1=published, 2=archived
          created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
          updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
        );
        CREATE INDEX IF NOT EXISTS idx_articles_author_time ON public.articles(author_id, created_at DESC);
        CREATE INDEX IF NOT EXISTS idx_articles_status_time ON public.articles(status, created_at DESC);
        )SQL");
        if (!r.ok) {
            usub::ulog::error("PgQuery failed in: {}, code={} | sqlstate='{}' | message='{}'",
                              make_location_string(), toString(r.code),
                              r.err_detail.sqlstate, r.err_detail.message);
            co_return;
        }
    } {
        auto r = co_await pool->query_awaitable(R"SQL(
        CREATE TABLE IF NOT EXISTS public.comments (
          id BIGSERIAL PRIMARY KEY,
          article_id BIGINT NOT NULL REFERENCES public.articles(id) ON DELETE CASCADE,
          author_id BIGINT REFERENCES public.users(id) ON DELETE SET NULL,
          body TEXT NOT NULL,
          created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        );
        CREATE INDEX IF NOT EXISTS idx_comments_article_time ON public.comments(article_id, created_at ASC);
        )SQL");
        if (!r.ok) {
            usub::ulog::error("PgQuery failed in: {}, code={} | sqlstate='{}' | message='{}'",
                              make_location_string(), toString(r.code),
                              r.err_detail.sqlstate, r.err_detail.message);
            co_return;
        }
    }
}
```

Some readers can wonder about what we've written. Let's take a closer look:

1. `usub::pg::RouteHint hint{...}; auto* pool = connector.route(hint);` selects a node (and its connection pool) for the query.
    - `hint.kind` describes the query type. `DDL` and `Write` are always routed to the primary node. `Read` may be routed to a replica, depending on consistency and health checks.
    - `hint.consistency` controls whether reads are allowed from replicas:
        - `Strong` routes reads to the primary.
        - `Eventual` allows reads from replicas.
        - `BoundedStaleness` allows reads from replicas only if their replication lag is within the configured threshold.
    - `connector.route(hint)` applies these rules, picks the best node (replica first for eligible reads, otherwise primary), and returns a `PgPool*` for executing the query.
2. `usub::uvent::task::Awaitable<void> test_db_query(usub::pg::PgPool& pool)` is a coroutine. Since the return type is `Awaitable<void>`, treat it as “a `void` function that may suspend and must be awaited with `co_await`.

3.
```cpp
auto r = co_await pool.query_awaitable(R"SQL(
  CREATE TABLE IF NOT EXISTS public.users (
    id BIGSERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    roles TEXT[] NOT NULL DEFAULT '{}',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
  );
)SQL");
```

sends an SQL query to PostgreSQL. The coroutine writes the query to the socket, then suspends until the database responds. While suspended, it doesn’t busy-wait and doesn’t consume CPU, so the event loop thread can run other coroutines.

4. 

```cpp
        if (!r.ok) {
            usub::ulog::error("PgQuery failed in: {}, code={} | sqlstate='{}' | message='{}'",
                              make_location_string(), toString(r.code),
                              r.err_detail.sqlstate, r.err_detail.message);
            co_return;
        }
```

responds for handling errors from database correctly. It'll provided detailed log of an error if it's returned by the database.

## Creating connection pool
Instead of simple example I decided to provided two variants of pool creation.

### Basic connection pool creation

```cpp
usub::pg::PgPool pool(
    "localhost", // host
    "12432",     // port
    "dev",       // user
    "devdb",     // database
    "devpass",   // password
    32           // max pool size (max connections)
);
```

If all connections are busy, acquire will suspend the current coroutine until another coroutine returns a connection back to the pool.

### Advanced connection pool

```cpp
    usub::pg::PgConnector router_main =
            usub::pg::PgConnectorBuilder{}
            .node("p1", "localhost", "5432", "dev", "devdb", "devpass", usub::pg::NodeRole::Primary, 1,
                  32)
            .primary_failover({"p1"})
            .default_consistency(usub::pg::Consistency::Eventual)
            .bounded_staleness(std::chrono::milliseconds{150}, 0)
            .read_my_writes_ttl(std::chrono::milliseconds{500})
            .pool_limits(64, 16)
            .health(60000, 120, "SELECT 1")
            .build();
```
### PgConnectorBuilder methods

- `node(name, host, port, user, db, password, role, weight = 1, max_pool = 32)`
  Adds a PostgreSQL endpoint to the connector configuration.
    - `name` — node identifier used in routing/failover lists.
    - `role` — node role (`Primary` / `Replica` / etc.).
    - `weight` — weight for read-balancing between nodes of the same role (higher = chosen more often).
    - `max_pool` — maximum number of connections for this node’s pool.
- `primary_failover({ "p1", "p2", ... })`
  Sets the ordered list of node names to try when the primary is unavailable.
- `default_consistency(Consistency c)`
  Sets the default routing consistency for queries (for example, prefer primary vs allow replicas).
- `bounded_staleness(ms, lsn = 0)`
  Configures “bounded staleness” routing: allow reading from replicas only if their replication lag is within `ms`
  (and optionally not older than a given `lsn`, if you use LSN-based checks).
- `read_my_writes_ttl(ttl)`
  Enables “read-your-writes” behavior for a short time window after a write: for `ttl`, reads are forced to the
  primary (or to a node that is guaranteed up-to-date, depending on your implementation).
- `pool_limits(def_max, olap_max)`
  Sets global connection limits (two classes). Typical meaning:
    - `def_max` — max connections for regular/OLTP traffic
    - `olap_max` — separate cap for heavy/analytical queries
      (Exact semantics depend on how `limits` is used in your connector.)
- `timeouts(connect, qread, qwrite)`
  Sets client-side timeouts in milliseconds:
    - `connect` — connection establishment timeout
    - `qread` — socket read timeout for query responses
    - `qwrite` — socket write timeout for sending requests
- `health(interval_ms, lag_thr_ms, probe_sql = "SELECT 1")`
  Enables health checking:
    - `interval_ms` — how often to probe nodes
    - `lag_thr_ms` — replication lag threshold used to mark replicas as “too stale”
    - `probe_sql` — SQL used for RTT/availability probing (defaults to `SELECT 1`)
- `build()`
  Validates the configuration (via `validate()`) and returns a constructed `PgConnector`.
- `config() const`
  Returns the current builder configuration (useful for debugging/printing before `build()`).

> In our project we'll use and advanced one.

## Starting migration and health-probe
`uvent` provides method to spawn coroutines via `co_spawn`. To spawn migration-coroutine and health probe simply add two line of code under `router_main`:

```cpp
int main() {
    usub::ulog::ULogInit cfg{
        .trace_path = nullptr, // nullptr means stdout
        .debug_path = nullptr, // -//-
        .info_path = nullptr, // -//-
        .warn_path = nullptr, // -//-
        .error_path = nullptr, // -//-
        .critical_path = nullptr, // -//-
        .fatal_path = nullptr, // -//-
        .flush_interval_ns = 5'000'000'000ULL, // 5 seconds
        .queue_capacity = 1024,
        .batch_size = 512,
        .enable_color_stdout = true,
        .json_mode = false,
        .track_metrics = true
    };

    usub::ulog::init(cfg);

    auto config_path = "../../config.toml";
    usub::server::Server server(config_path);
    usub::ulog::info("Server configured with TOML at: {}", config_path);

    usub::pg::PgConnector router_main =
            usub::pg::PgConnectorBuilder{}
            .node("p1", "localhost", "5432", "dev", "devdb", "devpass", usub::pg::NodeRole::Primary, 1,
                  32)
            .primary_failover({"p1"})
            .default_consistency(usub::pg::Consistency::Eventual)
            .bounded_staleness(std::chrono::milliseconds{150}, 0)
            .read_my_writes_ttl(std::chrono::milliseconds{500})
            .pool_limits(64, 16)
            .health(60000, 120, "SELECT 1")
            .build();
    // spawning coroutines
    usub::uvent::system::co_spawn(router_main.start_health_loop());
    usub::uvent::system::co_spawn(migration_coroutine(router_main));


    return 0;
}
```

# uRedis configuration

`RedisClusterConfig` defines how the client bootstraps into a Redis/KeyDB **cluster** and how it behaves after it has discovered the topology.

## Minimal setup

```cpp
#include "uredis/RedisClusterClient.h"

usub::uredis::RedisClusterConfig uredis_cfg;
uredis_cfg.seeds = {{"127.0.0.1", 6379}};

usub::uredis::RedisClusterClient uredis_client{uredis_cfg};
```

### What `seeds` does

`seeds` is a **bootstrap** list. The client connects to the first reachable seed node and then **discovers the rest of the cluster automatically** (slot map + node list).

> In cluster mode you can specify only one node — other nodes will be discovered automatically.

Under the hood the flow is typically:

1. Connect to a seed node.
2. Request cluster topology (e.g., slot map / nodes list).
3. Build an internal routing table: `hash-slot -> master node`.
4. Open (or lazily open) connections/pools to discovered nodes.
5. Route commands based on the key’s hash slot.
6. Handle redirects (`MOVED` / `ASK`) when the cluster topology changes.

## Cluster vs standalone

This config is for **cluster** usage. If your server runs in standalone mode, cluster discovery calls will fail (common error: `cluster support disabled`) however it will continue working in standalone mode.

# First encounter with `unet`

## Probes

### /healthz

In my opinion, the best example for getting acquainted with unet would be health-probe:

```cpp
    server.handle("GET", "/healthz", [&](usub::server::protocols::http::Request &request,
                      usub::server::protocols::http::Response &response
            ) -> usub::uvent::task::Awaitable<void> {
        response.setStatus(200).addHeader("Content-Type", "application/json").setBody(R"({"status": true})");
        co_return;    
    });
```

As you may have noticed, one of the important features of unet that you're able to set up any request method you wish. It's [specified](https://httpwg.org/specs/rfc9110.html#method.overview) in HTTP RFC but not many web-frameworks supports it. Might be especially useful for [such cases](https://www.iana.org/assignments/http-methods/http-methods.xhtml). In unet implementation we limit method token size to 256 characters, if the request method is longer, the server will decline this request.

As an additional feature, you can define multiple methods

```cpp
    server.handle({"GET", "POST", "CUSTOM"}, "/endpoint", [&](usub::server::protocols::http::Request &request,
                      usub::server::protocols::http::Response &response
            ) -> usub::uvent::task::Awaitable<void> {
        std::string response_json = "{\"request_method\" : \"" + request.getRequestMethod() + "\"}";
        response.setStatus(200).addHeader("Content-Type", "application/json").setBody(response_json));
        co_return;    
    });
```

Or in case you want the unet to accept any token you can use asterisk(*) symbol.

```cpp
    server.handle("*", "/endpoint", [&](usub::server::protocols::http::Request &request,
                      usub::server::protocols::http::Response &response
            ) -> usub::uvent::task::Awaitable<void> {
        std::string response_json = "{\"request_method\" : \"" + request.getRequestMethod() + "\"}";
        response.setStatus(200).addHeader("Content-Type", "application/json").setBody(response_json));
        co_return;    
    });
```

### /startup
Another one way to create handler is:
```cpp
usub::uvent::task::Awaitable<void> startup_probe(usub::server::protocols::http::Request &request,
                      usub::server::protocols::http::Response &response) {
    response.setStatus(204);
    co_return;
}
```
In that example we've created static method and can simply pass using such code:
```cpp
server.handle("GET", "/startup", startup_probe);
```

## Integration with UPQ and uRedis
Since we've done implementing probes, we can start implementing methods to control users (e.g. create, read, delete, update)

Basically we need to create `UserHandler` class and specify basic methods as was mentioned above (`UserHandler.h`):
```cpp
#ifndef ARTICLE_USERHANDLER_H
#define ARTICLE_USERHANDLER_H

#include <upq/PgRouting.h>
#include <ulog/ulog.h>
#include "server/server.h"
#include "utils/LoggingUtils.h"

namespace article::handler {
    class UserHandler {
    public:
        UserHandler(usub::pg::PgConnector &connector);

        ServerHandler createUser(usub::server::protocols::http::Request &request,
                                 usub::server::protocols::http::Response &response);

        usub::uvent::task::Awaitable<void> updateUser(usub::server::protocols::http::Request &request,
                                 usub::server::protocols::http::Response &response);
        
        ServerHandler loadUser(usub::server::protocols::http::Request &request,
                                 usub::server::protocols::http::Response &response);
        
        ServerHandler deleteUser(usub::server::protocols::http::Request &request,
                                 usub::server::protocols::http::Response &response);
    private:
        usub::pg::PgConnector &connector_;
    };
}

#endif //ARTICLE_USERHANDLER_H
```
Also we need to create source file (`UserHandler.cpp`):
```cpp
#include "handlers/UserHandler.h"

namespace article::handler {
    ServerHandler UserHandler::createUser(usub::server::protocols::http::Request &request,
                                          usub::server::protocols::http::Response &response) {
        try {
            usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), request.getBody());
        } catch (std::exception &e) {
            usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        }
    }

    usub::uvent::task::Awaitable<void> UserHandler::updateUser(usub::server::protocols::http::Request &request,
        usub::server::protocols::http::Response &response) {
        try {
            usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), request.getBody());
        } catch (std::exception &e) {
            usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        }
    }

    ServerHandler UserHandler::loadUser(usub::server::protocols::http::Request &request,
        usub::server::protocols::http::Response &response) {
        try {
            usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), request.getBody());
        } catch (std::exception &e) {
            usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        }
    }

    ServerHandler UserHandler::deleteUser(usub::server::protocols::http::Request &request,
        usub::server::protocols::http::Response &response) {
        try {
            usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), request.getBody());
        } catch (std::exception &e) {
            usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        }
    }
}
```

I've added some basic code to correctly handle requests on fault. Now we have come to the point where we need to create a DTO. First of all let's specify folder structure (tree log):

```
.
├── CMakeLists.txt
├── config.toml
├── docker-compose.yaml
├── include
│   ├── api
│   │   └── dto
│   │       ├── requests
│   │       │   ├── Article.h
│   │       │   └── User.h
│   │       └── responses
│   │           ├── Article.h
│   │           └── User.h
│   ├── handlers
│   │   └── UserHandler.h
│   └── utils
│       └── LoggingUtils.h
├── keydb.conf
└── src
    ├── handlers
    │   └── UserHandler.cpp
    └── main.cpp
```

Our DTOs will be stored in `include/api/dto`.
- `include/api/dto/requests` — payloads you receive from clients (JSON → C++ struct)
- `include/api/dto/responses` — payloads you send back (C++ struct → JSON).
