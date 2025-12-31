# Asynchronous backend on modern C++ 23.

First of all I need to tell that this article is about our C++ 23 stack currently used in production and produced good
performance. Mostly all our libraries based on top of three basic libraries:

- [uvent](https://github.com/Usub-development/uvent) – asynchronous cross-platform engine which provides coroutines
  wrappers (like in [boost.asio](https://www.boost.org/doc/libs/latest/doc/html/boost_asio.html)) and cross-platform I/O
  for all of our libraries.
- [unet](https://github.com/Usub-development/unet) – asynchronous web-server based on `uvent` which allows to use
  coroutines as handlers for endpoints (a good advantage, to be explained later).
- [ureflect](https://github.com/Usub-development/ureflect) – compile time reflection (without need to use a
  preprocessor).

In this article we'll also use some other our libraries:

- [upq](https://github.com/Usub-development/upq) – asynchronous PostgreSQL client library build on top
  of [libpq](https://www.postgresql.org/docs/current/libpq.html), `uvent`, `ureflect`. Provides ability to query data
  from data base with or without reflection (JSON/JSONB also can be parsed into datastructures via reflection).
- [uredis](https://github.com/Usub-development/uredis) – asynchronous redis library build on top of `uvent` with own
  RESP3 implementation.
- [ulog](https://github.com/Usub-development/ulog) – logger inspired by [spdlog](https://github.com/gabime/spdlog) build
  on top of `uvent`. Instead of using own thread spawns flushing coroutine.
- [ujson](https://github.com/Usub-development/ujson) – simple json library with reflection.

> This article isn't a guide how to get into coroutines however it shows what can be done by using them in correct way.

> This article is only for educational purpose only. Don't use endpoint handlers implementations in real production.

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
    command: [ "keydb-server", "/etc/keydb/keydb.conf" ]
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

After setting up our databases we're able to begin configuring our project. Before we start writing code let's setup
`CMakeLists.txt` file correctly:

```cmake
cmake_minimum_required(VERSION 3.27)
project(article)

set(CMAKE_CXX_STANDARD 23)
set(UREDIS_BUILD_EXAMPLES OFF)
set(UREDIS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UREDIS_BUILD_STATIC ON CACHE BOOL "" FORCE)
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

When used in logs, `make_location_string` provides the most detailed indication of the call location, which is very
useful for detecting errors.

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
- `queue_capacity` – capacity of lock-free queue which is used as log storage by default. If capacity exceeded it'll
  fallback to queue with mutex. You can track fallback metrics like that:

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
{
  "time": "2025-10-28 12:03:44.861",
  "thread": 3,
  "level": "I",
  "msg": "starting event loop..."
}
```

- `track_metrics` – should ulog track fallback metrics or not.

> `usub::ulog::init(cfg);` initializes the global logger. Call it before any logging; otherwise the program may crash (
> e.g., segfault).

Before we start creating server instance it's necessary to say that uvent is a public dependency, so you're allowed to
use all of it's functionality with coroutines from each of our library or implementing your own logic in coroutines.

### Creating `unet` instance

Let's create toml [config](https://usub-development.github.io/unet/config/) for `unet`:

```toml
[server]
threads = 4
ip_addr = "0.0.0.0"
timeout = 5000

[[listener]]
port = 17000
ssl = false
```

Now we need to pass path to config to `unet` instance:

```cpp
auto config_path = "../../config.toml";
usub::server::Server server(config_path);
usub::ulog::info("Server configured with TOML at: {}", config_path);
```

## Creating migration coroutine

For migration, I decided to create an example without transactions because I decided to build UPQ use cases
sequentially—from simple to more complex. Here is the code:

```cpp
usub::uvent::task::Awaitable<void> migration_coroutine(usub::pg::PgConnector &connector) {
    usub::pg::RouteHint hint{
        .kind = usub::pg::QueryKind::Write,
        .consistency = usub::pg::Consistency::Eventual
    };
    auto pool = connector.route(hint); {
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
    }
}
```

Some readers can wonder about what we've written. Let's take a closer look:

1. `usub::pg::RouteHint hint{...}; auto* pool = connector.route(hint);` selects a node (and its connection pool) for the
   query.
    - `hint.kind` describes the query type. `DDL` and `Write` are always routed to the primary node. `Read` may be
      routed to a replica, depending on consistency and health checks.
    - `hint.consistency` controls whether reads are allowed from replicas:
        - `Strong` routes reads to the primary.
        - `Eventual` allows reads from replicas.
        - `BoundedStaleness` allows reads from replicas only if their replication lag is within the configured
          threshold.
    - `connector.route(hint)` applies these rules, picks the best node (replica first for eligible reads, otherwise
      primary), and returns a `PgPool*` for executing the query.
2. `usub::uvent::task::Awaitable<void> test_db_query(usub::pg::PgPool& pool)` is a coroutine. Since the return type is
   `Awaitable<void>`, treat it as “a `void` function that may suspend and must be awaited with `co_await`.

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

sends an SQL query to PostgreSQL. The coroutine writes the query to the socket, then suspends until the database
responds. While suspended, it doesn’t busy-wait and doesn’t consume CPU, so the event loop thread can run other
coroutines.

4.

```cpp
        if (!r.ok) {
            usub::ulog::error("PgQuery failed in: {}, code={} | sqlstate='{}' | message='{}'",
                              make_location_string(), toString(r.code),
                              r.err_detail.sqlstate, r.err_detail.message);
            co_return;
        }
```

responds for handling errors from database correctly. It'll provided detailed log of an error if it's returned by the
database.

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

If all connections are busy, acquire will suspend the current coroutine until another coroutine returns a connection
back to the pool.

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

`uvent` provides method to spawn coroutines via `co_spawn`. To spawn migration-coroutine and health probe simply add two
line of code under `router_main`:

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

`RedisClusterConfig` defines how the client bootstraps into a Redis/KeyDB **cluster** and how it behaves after it has
discovered the topology.

## Minimal setup

```cpp
#include "uredis/RedisClusterClient.h"

usub::uredis::RedisClusterConfig uredis_cfg;
uredis_cfg.seeds = {{"127.0.0.1", 6379}};
uredis_cfg.password = "devpass";

usub::uredis::RedisClusterClient uredis_client{uredis_cfg};
```

### What `seeds` does

`seeds` is a **bootstrap** list. The client connects to the first reachable seed node and then **discovers the rest of
the cluster automatically** (slot map + node list).

> In cluster mode you can specify only one node — other nodes will be discovered automatically.

Under the hood the flow is typically:

1. Connect to a seed node.
2. Request cluster topology (e.g., slot map / nodes list).
3. Build an internal routing table: `hash-slot -> master node`.
4. Open (or lazily open) connections/pools to discovered nodes.
5. Route commands based on the key’s hash slot.
6. Handle redirects (`MOVED` / `ASK`) when the cluster topology changes.

## Cluster vs standalone

This config is for **cluster** usage. If your server runs in standalone mode, cluster discovery calls will fail (common
error: `cluster support disabled`) however it will continue working in standalone mode.

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

As you may have noticed, one of the important features of unet that you're able to set up any request method you wish.
It's [specified](https://httpwg.org/specs/rfc9110.html#method.overview) in HTTP RFC but not many web-frameworks supports
it. Might be especially useful for [such cases](https://www.iana.org/assignments/http-methods/http-methods.xhtml). In
unet implementation we limit method token size to 256 characters, if the request method is longer, the server will
decline this request.

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

Since we've done implementing probes, we can start implementing methods to control users (e.g. create, read,
update)

Basically we need to create `UserHandler` class and specify basic methods as was mentioned above (`UserHandler.h`):

```cpp
#ifndef ARTICLE_USERHANDLER_H
#define ARTICLE_USERHANDLER_H

#include <upq/PgRouting.h>
#include <ulog/ulog.h>
#include "server/server.h"
#include "utils/LoggingUtils.h"
#include "utils/HttpError.h"
#include "utils/Hash.h"
#include "api/dto/requests/User.h"
#include "api/dto/responses/User.h"
#include <uredis/RedisClusterClient.h>

namespace article::handler {
    class UserHandler {
    public:
        UserHandler(usub::pg::PgConnector &connector, usub::uredis::RedisClusterClient& redis_cluster_client);

        ServerHandler createUser(usub::server::protocols::http::Request &request,
                                 usub::server::protocols::http::Response &response);

        usub::uvent::task::Awaitable<void> updateUser(usub::server::protocols::http::Request &request,
                                 usub::server::protocols::http::Response &response);

        ServerHandler loadUser(usub::server::protocols::http::Request &request,
                                 usub::server::protocols::http::Response &response);

    private:
        usub::pg::PgConnector &connector_;
        usub::uredis::RedisClusterClient& redis_cluster_client_;
    };
}

#endif //ARTICLE_USERHANDLER_H
```

Also we need to create source file (`UserHandler.cpp`):

```cpp
#include "handlers/UserHandler.h"

namespace article::handler {
    UserHandler::UserHandler(usub::pg::PgConnector &connector,
                             usub::uredis::RedisClusterClient &redis_cluster_client) : connector_(connector),
        redis_cluster_client_(redis_cluster_client) {
    }

    ServerHandler UserHandler::createUser(usub::server::protocols::http::Request &request,
                                          usub::server::protocols::http::Response &response) {
        try {
            auto req_body = request.getBody();
            usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), req_body);
        } catch (std::exception &e) {
            usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        }
    }

    usub::uvent::task::Awaitable<void> UserHandler::updateUser(usub::server::protocols::http::Request &request,
        usub::server::protocols::http::Response &response) {
        try {
            auto req_body = request.getBody();
            usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), req_body);
        } catch (std::exception &e) {
            usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        }
    }

    ServerHandler UserHandler::loadUser(usub::server::protocols::http::Request &request,
        usub::server::protocols::http::Response &response) {
        try {
            auto req_body = request.getBody();
            usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), req_body);
        } catch (std::exception &e) {
            usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        }
    }
}
```

I've added some basic code to correctly handle requests on fault. However, for a detailed return of errors, we need to
create an additional data structure. (`include/utils/HttpError.h`):

```cpp
#ifndef ARTICLE_HTTPERROR_H
#define ARTICLE_HTTPERROR_H

#include <string>
#include <optional>

namespace article::utils::errors {
    struct RequestError {
        int error_code;
        std::string message;
        std::optional<std::string> detail;
    };
}

#endif //ARTICLE_HTTPERROR_H
```

> Don't forget to add `#include` into `UserHandler.h`

### DTO

Now we have come to the point where we need to create a DTO. First of all let's specify folder structure (tree log):

```
.
├── CMakeLists.txt
├── LICENSE
├── README.md
├── config.toml
├── docker-compose.yaml
├── include
│   ├── api
│   │   └── dto
│   │       ├── requests
│   │       │   └── User.h
│   │       └── responses
│   │           └── User.h
│   ├── handlers
│   │   └── UserHandler.h
│   └── utils
│       ├── HttpError.h
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

Let's proceed with the implementation of User DTO (e.g. `requests/User.h`):

```cpp
#ifndef ARTICLE_USER_REQUEST_H
#define ARTICLE_USER_REQUEST_H

#include <string>
#include <vector>
#include <ujson/ujson.h>
#include <upq/PgTypes.h>

#include "uvent/net/SocketMetadata.h"

namespace article::dto {
    enum class Roles {
        User, Admin
    };

    struct CreateUser {
        std::string name;
        std::string password;
        std::vector<Roles> roles;
    };

    struct UpdateUser {
        std::string id;
        std::string name;
        std::string old_password;
        std::string new_password;
        std::vector<Roles> roles;
    };

    struct LoadUser {
        std::string id;
    };
    
    struct DeleteUser {
        std::string id;
    };
}

template<>
struct ujson::enum_meta<article::dto::Roles> {
    using enum article::dto::Roles;
    static inline constexpr auto items = enumerate<User, Admin>();
};

template<>
struct usub::pg::detail::upq::enum_meta<article::dto::Roles> {
    using enum article::dto::Roles;
    static constexpr auto mapping = enumerate<
        User, Admin
    >();
};

#endif //ARTICLE_USER_REQUEST_H
```

As you can see, DTO is almost simple excluding section in the bottom of the file. `enum_meta` from both ujson and upq is
used to handle enumerates correctly due to that compile-time reflection isn't a kind of magic so we are forced to write
such constructions unfortunately. However they allows us to be free from writing methods like
`std::string to_string(enum_type);` and enum values will be automatically converted into strings while serializing
object and into enums while deserializing.

> `!For experts`: enums will be pushed to the database as a
> string ([OID 25](https://jdbc.postgresql.org/documentation/publicapi/constant-values.html) if single
> element, [OID 1009]((https://jdbc.postgresql.org/documentation/publicapi/constant-values.html)) if it's an array).

Response DTO:

```cpp
#ifndef ARTICLE_USER_RESPONSE_H
#define ARTICLE_USER_RESPONSE_H

#include "api/dto/requests/User.h"

namespace article::dto::response {
    struct CreateUser {
        int id;
    };

    struct User {
        int id;
        std::string name;
        std::string password_hash;
        std::vector<Roles> roles;
        std::string created_at;
    };

    struct LoadUser {
        std::vector<User> data;
    };
}

#endif //ARTICLE_USER_RESPONSE_H
```

## Hash

Initially we need to implement user creation we need to create hash-function for passwords (`include/utils/Hash.h`):

```cpp
#ifndef ARTICLE_HASH_H
#define ARTICLE_HASH_H

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/core_names.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace article::hash {
    static std::string b64_encode(const uint8_t *data, size_t len) {
        std::string out;
        out.resize(4 * ((len + 2) / 3));
        int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                reinterpret_cast<const unsigned char *>(data),
                                static_cast<int>(len));
        if (n < 0) throw std::runtime_error("EVP_EncodeBlock failed");
        out.resize(static_cast<size_t>(n));
        return out;
    }

    static std::vector<uint8_t> b64_decode(std::string_view s) {
        std::vector<uint8_t> out(3 * (s.size() / 4) + 3);
        int n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                reinterpret_cast<const unsigned char *>(s.data()),
                                static_cast<int>(s.size()));
        if (n < 0) throw std::runtime_error("EVP_DecodeBlock failed");

        size_t pad = 0;
        if (!s.empty() && s.back() == '=') pad++;
        if (s.size() > 1 && s[s.size() - 2] == '=') pad++;

        size_t real = static_cast<size_t>(n);
        if (pad) real -= pad;
        out.resize(real);
        return out;
    }

    static std::vector<std::string_view> split_sv(std::string_view s, char d) {
        std::vector<std::string_view> parts;
        size_t start = 0;
        while (start <= s.size()) {
            size_t pos = s.find(d, start);
            if (pos == std::string_view::npos) {
                parts.push_back(s.substr(start));
                break;
            }
            parts.push_back(s.substr(start, pos - start));
            start = pos + 1;
        }
        return parts;
    }

    struct KdfDeleter {
        void operator()(EVP_KDF *p) const noexcept { EVP_KDF_free(p); }
    };

    struct KdfCtxDeleter {
        void operator()(EVP_KDF_CTX *p) const noexcept { EVP_KDF_CTX_free(p); }
    };

    static std::vector<uint8_t> pbkdf2_hmac_sha256(std::string_view password,
                                                   const uint8_t *salt, size_t salt_len,
                                                   uint32_t iterations,
                                                   size_t dk_len) {
        std::unique_ptr<EVP_KDF, KdfDeleter> kdf(EVP_KDF_fetch(nullptr, "PBKDF2", nullptr));
        if (!kdf) throw std::runtime_error("EVP_KDF_fetch(PBKDF2) failed");

        std::unique_ptr<EVP_KDF_CTX, KdfCtxDeleter> ctx(EVP_KDF_CTX_new(kdf.get()));
        if (!ctx) throw std::runtime_error("EVP_KDF_CTX_new failed");

        char digest_name[] = "SHA256";
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                              const_cast<char *>(password.data()),
                                              password.size()),
            OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                              const_cast<uint8_t *>(salt),
                                              salt_len),
            OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
            OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                             digest_name,
                                             0),
            OSSL_PARAM_construct_end()
        };

        std::vector<uint8_t> out(dk_len);
        if (EVP_KDF_derive(ctx.get(), out.data(), out.size(), params) != 1)
            throw std::runtime_error("EVP_KDF_derive failed");

        return out;
    }

    // pbkdf2$sha256$iters$salt_b64$dk_b64
    static std::string hash_password(std::string_view password,
                                     uint32_t iterations = 200'000,
                                     size_t salt_len = 16,
                                     size_t dk_len = 32) {
        std::vector<uint8_t> salt(salt_len);
        if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
            throw std::runtime_error("RAND_bytes failed");

        auto dk = pbkdf2_hmac_sha256(password, salt.data(), salt.size(), iterations, dk_len);

        return "pbkdf2$sha256$" + std::to_string(iterations) + "$" +
               b64_encode(salt.data(), salt.size()) + "$" +
               b64_encode(dk.data(), dk.size());
    }

    static bool verify_password(std::string_view password, std::string_view stored) {
        auto parts = split_sv(stored, '$');
        if (parts.size() != 5) return false;
        if (parts[0] != "pbkdf2") return false;
        if (parts[1] != "sha256") return false;

        uint32_t iterations = 0;
        try { iterations = static_cast<uint32_t>(std::stoul(std::string(parts[2]))); } catch (...) { return false; }
        if (iterations == 0) return false;

        std::vector<uint8_t> salt, expected;
        try {
            salt = b64_decode(parts[3]);
            expected = b64_decode(parts[4]);
        } catch (...) {
            return false;
        }

        auto dk = pbkdf2_hmac_sha256(password, salt.data(), salt.size(), iterations, expected.size());
        if (dk.size() != expected.size()) return false;

        return CRYPTO_memcmp(dk.data(), expected.data(), dk.size()) == 0;
    }
}

#endif //ARTICLE_HASH_H
```

## Endpoint handlers

Endpoint handler for user creation:

```cpp
ServerHandler UserHandler::createUser(usub::server::protocols::http::Request &request,
                                      usub::server::protocols::http::Response &response) {
    try {
        auto req_body = request.getBody();
        usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), req_body);

        auto json_body = ujson::try_parse<dto::CreateUser>(req_body);
        if (!json_body) {
            // Checking if JSON is correct.
            auto &e = json_body.error();
            usub::ulog::error("error: {1}, near: {0}, path: {2}", e.near(req_body), e.msg, e.path);
        }

        auto &data = json_body.value();

        // Creating hint for transaction (useful for PostgreSQL with replicas)
        usub::pg::RouteHint hint{
            .kind = usub::pg::QueryKind::Write,
            .consistency = usub::pg::Consistency::Eventual
            // More info: https://usub-development.github.io/upq/routing/#consistency-policies
        };
        auto pool = this->connector_.route(hint);

        usub::pg::PgTransaction txn{
            pool,
            {
                .isolation = usub::pg::TxIsolationLevel::Default,
                .read_only = false,
                .deferrable = false
            }
        };

        if (auto err_begin = co_await txn.begin_errored(); err_begin) {
            co_await txn.finish();
            const auto &e = err_begin.value();
            usub::ulog::error(
                "Error in {}: txn begin failed | code={} | message='{}' | sqlstate='{}' | "
                "detail='{}'",
                make_location_string(), toString(e.code), e.error, e.err_detail.sqlstate,
                e.err_detail.message);

            utils::errors::RequestError error{
                .error_code = 1099,
                .message =
                "Something went wrong"
            };
            response.setStatus(400)
                    .setBody(ujson::dump(error))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        auto create_r = co_await txn.query(
            R"(INSERT INTO users(name, password_hash, roles) VALUES ($1, $2, $3) RETURNING id;)",
            data.name, hash::hash_password(data.password), data.roles);

        if (!create_r.ok) {
            co_await txn.finish();
            usub::ulog::error(
                "PgQuery failed in: {}, code={} | sqlstate='{}' | message='{}' | "
                "query_empty='{}'",
                make_location_string(), toString(create_r.code),
                create_r.err_detail.sqlstate, create_r.err_detail.message,
                create_r.empty());
            utils::errors::RequestError err{
                .error_code = 1004,
                .message = "Failed to create user"
            };
            response.setStatus(422)
                    .setBody(ujson::dump(err))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        if (!co_await txn.commit()) {
            usub::ulog::error("Commit failed: {}", make_location_string());
            utils::errors::RequestError err{
                .error_code = 1099,
                .message = "Something went wrong"
            };
            response.setStatus(422)
                    .setBody(ujson::dump(err))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        dto::response::CreateUser create_user{
            .id = create_r.get<int>(0, "id").value_or(-1)
            // from first (index: 0) row trying to get value with column name `id`
        };

        response.setStatus(200)
                .setBody(ujson::dump(create_user))
                .addHeader("Content-Type", "application/json; charset=UTF-8");
    } catch (std::exception &e) {
        usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        utils::errors::RequestError error{
            .error_code = 1099,
            .message =
            "Something went wrong"
        };
        response.setStatus(400)
                .setBody(ujson::dump(error))
                .addHeader("Content-Type", "application/json; charset=UTF-8");
    }
}
```

Endpoint handler for user update:

```cpp
usub::uvent::task::Awaitable<void> UserHandler::updateUser(usub::server::protocols::http::Request &request,
                                                           usub::server::protocols::http::Response &response) {
    try {
        auto req_body = request.getBody();
        usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), req_body);

        auto json_body = ujson::try_parse<dto::UpdateUser>(req_body);
        if (!json_body) {
            // Checking if JSON is correct.
            auto &e = json_body.error();
            usub::ulog::error("error: {1}, near: {0}, path: {2}", e.near(req_body), e.msg, e.path);
        }

        auto &data = json_body.value();

        // Creating hint for transaction (useful for PostgreSQL with replicas)
        usub::pg::RouteHint hint{
            .kind = usub::pg::QueryKind::Write,
            .consistency = usub::pg::Consistency::Eventual
            // More info: https://usub-development.github.io/upq/routing/#consistency-policies
        };
        auto pool = this->connector_.route(hint);

        usub::pg::PgTransaction txn{
            pool,
            {
                .isolation = usub::pg::TxIsolationLevel::Default,
                .read_only = false,
                .deferrable = false
            }
        };

        if (auto err_begin = co_await txn.begin_errored(); err_begin) {
            co_await txn.finish();
            const auto &e = err_begin.value();
            usub::ulog::error(
                "Error in {}: txn begin failed | code={} | message='{}' | sqlstate='{}' | "
                "detail='{}'",
                make_location_string(), toString(e.code), e.error, e.err_detail.sqlstate,
                e.err_detail.message);

            utils::errors::RequestError error{
                .error_code = 1099,
                .message =
                "Something went wrong"
            };
            response.setStatus(400)
                    .setBody(ujson::dump(error))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        auto user_r = co_await txn.query(R"(SELECT password_hash FROM users WHERE id = $1)", data.id);

        if (!user_r.ok) {
            co_await txn.finish();
            usub::ulog::error(
                "PgQuery failed in: {}, code={} | sqlstate='{}' | message='{}' | "
                "query_empty='{}'",
                make_location_string(), toString(user_r.code),
                user_r.err_detail.sqlstate, user_r.err_detail.message,
                user_r.empty());
            utils::errors::RequestError err{
                .error_code = 1004,
                .message = "Failed to load user"
            };
            response.setStatus(422)
                    .setBody(ujson::dump(err))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        auto p_hash_ex = user_r.get<std::string>(0, "password_hash");

        if (!p_hash_ex) {
            co_await txn.finish();
            usub::ulog::error("Password verify failed in: {}", make_location_string());
            utils::errors::RequestError err{
                .error_code = 1004,
                .message = "Failed to load user"
            };
            response.setStatus(422)
                    .setBody(ujson::dump(err))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        auto &p_hash = p_hash_ex.value();

        if (hash::verify_password(data.old_password, p_hash)) {
            utils::errors::RequestError err{
                .error_code = 1004,
                .message = "Wrong password"
            };
            response.setStatus(422)
                    .setBody(ujson::dump(err))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        auto update_r = co_await txn.query(
            R"(UPDATE users SET name = $2, password_hash = $3, roles = $4 WHERE id = $1;)",
            data.id,
            data.name,
            hash::hash_password(data.new_password),
            data.roles
        );

        if (!update_r.ok || update_r.rows_affected == 0) {
            co_await txn.finish();
            usub::ulog::error(
                "PgQuery failed in: {}, code={} | sqlstate='{}' | message='{}' | "
                "query_empty='{}'",
                make_location_string(), toString(update_r.code),
                update_r.err_detail.sqlstate, update_r.err_detail.message,
                update_r.empty());
            utils::errors::RequestError err{
                .error_code = 1004,
                .message = "Failed to update user"
            };
            response.setStatus(422)
                    .setBody(ujson::dump(err))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        if (!co_await txn.commit()) {
            usub::ulog::error("Commit failed: {}", make_location_string());
            utils::errors::RequestError err{
                .error_code = 1099,
                .message = "Something went wrong"
            };
            response.setStatus(422)
                    .setBody(ujson::dump(err))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        response.setStatus(204);
    } catch (std::exception &e) {
        usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        utils::errors::RequestError error{
            .error_code = 1099,
            .message =
            "Something went wrong"
        };
        response.setStatus(400)
                .setBody(ujson::dump(error))
                .addHeader("Content-Type", "application/json; charset=UTF-8");
    }
}
```

Endpoint handler for user loading:

```cpp
ServerHandler UserHandler::loadUser(usub::server::protocols::http::Request &request,
                                    usub::server::protocols::http::Response &response) {
    try {
        auto req_body = request.getBody();
        usub::ulog::trace("Received request in: {}, request body: {}", make_location_string(), req_body);

        auto json_body = ujson::try_parse<dto::LoadUser>(req_body);
        if (!json_body) {
            // Checking if JSON is correct.
            auto &e = json_body.error();
            usub::ulog::error("error: {1}, near: {0}, path: {2}", e.near(req_body), e.msg, e.path);
        }

        auto &data = json_body.value(); {
            const std::string key = std::string("user:").append(data.id);
            auto r2 = co_await this->redis_cluster_client_.command("GET", key);
            if (!r2) {
                const auto &e = r2.error();
                usub::ulog::error("redis GET failed in: {}, reason: {} {}",
                                  make_location_string(), (int) e.category, e.message);
                co_return;
            }

            if (!r2->is_null()) {
                std::string out_json = r2->as_string();
                response.setStatus(200)
                        .setBody(out_json)
                        .addHeader("Content-Type", "application/json; charset=UTF-8");
                co_return;
            }
        }

        // Creating hint for transaction (useful for PostgreSQL with replicas)
        usub::pg::RouteHint hint{
            .kind = usub::pg::QueryKind::Read,
            .consistency = usub::pg::Consistency::Eventual
            // More info: https://usub-development.github.io/upq/routing/#consistency-policies
        };
        auto pool = this->connector_.route(hint);

        usub::pg::PgTransaction txn{
            pool,
            {
                .isolation = usub::pg::TxIsolationLevel::Default,
                .read_only = true,
                .deferrable = false
            }
        };

        if (auto err_begin = co_await txn.begin_errored(); err_begin) {
            co_await txn.finish();
            const auto &e = err_begin.value();
            usub::ulog::error(
                "Error in {}: txn begin failed | code={} | message='{}' | sqlstate='{}' | "
                "detail='{}'",
                make_location_string(), toString(e.code), e.error, e.err_detail.sqlstate,
                e.err_detail.message);

            utils::errors::RequestError error{
                .error_code = 1099,
                .message =
                "Something went wrong"
            };
            response.setStatus(400)
                    .setBody(ujson::dump(error))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        auto user_r = co_await txn.query_reflect_expected<dto::response::User>(
            R"(SELECT id, name, password_hash, roles, created_at FROM users WHERE id = $1)", data.id);

        if (!user_r.has_value()) {
            co_await txn.finish();
            auto &error = user_r.error();
            usub::ulog::error(
                "PgQuery failed in: {}, code={} | sqlstate='{}' | message='{}'",
                make_location_string(),
                toString(error.code),
                error.err_detail.sqlstate, error.err_detail.message);
            utils::errors::RequestError err{
                .error_code = 1099,
                .message =
                "User not found"
            };
            response.setStatus(422).setBody(ujson::dump(err)).
                    addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        if (!co_await txn.commit()) {
            usub::ulog::error("Commit failed: {}", make_location_string());
            utils::errors::RequestError err{
                .error_code = 1099,
                .message = "Something went wrong"
            };
            response.setStatus(422)
                    .setBody(ujson::dump(err))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        dto::response::LoadUser out{
            .data = std::move(user_r.value())
        };

        usub::ulog::debug("In: {1}, Out: {0}", out, data);
        // For more details: https://github.com/Usub-development/ulog

        auto out_json = ujson::dump(out);

        auto r1 = co_await this->redis_cluster_client_.command(
            "SET", std::string("user:").append(std::to_string(out.data.at(0).id)), std::string_view(out_json),
            std::string_view("EX"), std::string_view("1800"));
        if (!r1 || !r1->is_simple_string() || r1->as_string() != "OK") {
            usub::ulog::error("redis SET failed in: {}, reason: {}",
                              make_location_string(), r1 ? r1->as_string() : "no response");
            utils::errors::RequestError err{
                .error_code = 1099,
                .message = "Something went wrong"
            };
            response.setStatus(422)
                    .setBody(ujson::dump(err))
                    .addHeader("Content-Type", "application/json; charset=UTF-8");
            co_return;
        }

        response.setStatus(200)
                .setBody(out_json)
                .addHeader("Content-Type", "application/json; charset=UTF-8");
    } catch (std::exception &e) {
        usub::ulog::error("Error in: {}, reason: {}", make_location_string(), e.what());
        utils::errors::RequestError error{
            .error_code = 1099,
            .message =
            "Something went wrong"
        };
        response.setStatus(400)
                .setBody(ujson::dump(error))
                .addHeader("Content-Type", "application/json; charset=UTF-8");
    }
}
```

We've created all necessary endpoint for user creating, updating and loading. We've used reflection from: `ujson`
(both serialize and deserialize), `upq`, `ulog` (in user loading, last log before keyword `catch`) and applied cache via
`uredis` which is optimized loading.

## Binding handlers to endpoints

The only two things left is create instance of `UserHandler` and bind all functions from it.
Firstly we'll create `UserHandler` instance right after `usub::uredis::RedisClusterClient` instance:

```cpp
    usub::uredis::RedisClusterClient uredis_client{uredis_cfg};
    article::handler::UserHandler user_handler(router_main);

    server.handle("GET", "/healthz", [&](usub::server::protocols::http::Request &request,
                                         usub::server::protocols::http::Response &response
          ) -> usub::uvent::task::Awaitable<void> {
                      response.setStatus(200).addHeader("Content-Type", "application/json").setBody(
                          R"({"status": true})");
                      co_return;
                  });
```

And that's how we'll bind our handlers:

```cpp
    server
    .handle("POST", R"(/api/user/create)",
            bind_handler<&article::handler::UserHandler::createUser>(user_handler));
    server
    .handle("POST", R"(/api/user/update)",
            bind_handler<&article::handler::UserHandler::updateUser>(user_handler));
    server
    .handle("POST", R"(/api/user/load)",
            bind_handler<&article::handler::UserHandler::loadUser>(user_handler));
```

And don't forget to call `run()` method from server instance. It'll start `uvent` loop (or loops if multithreading is
used):

```cpp
server.run();
```

Now our `main.cpp` should look like:

```cpp
#include <server/server.h>
#include <upq/PgRouting.h>
#include <upq/PgRoutingBuilder.h>
#include <ulog/ulog.h>
#include <uredis/RedisClusterClient.h>

#include "handlers/UserHandler.h"
#include "utils/LoggingUtils.h"

usub::uvent::task::Awaitable<void> migration_coroutine(usub::pg::PgConnector &connector) {
    usub::pg::RouteHint hint{
        .kind = usub::pg::QueryKind::Write,
        .consistency = usub::pg::Consistency::Eventual
    };
    auto pool = connector.route(hint); {
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
    }
}

usub::uvent::task::Awaitable<void> startup_probe(usub::server::protocols::http::Request &request,
                                                 usub::server::protocols::http::Response &response) {
    response.setStatus(204);
    co_return;
}

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

    auto config_path = "../config.toml";
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

    usub::uredis::RedisClusterConfig uredis_cfg;
    uredis_cfg.seeds = {{"127.0.0.1", 6379}};
    uredis_cfg.password = "devpass";

    usub::uredis::RedisClusterClient uredis_client{uredis_cfg};
    article::handler::UserHandler user_handler(router_main, uredis_client);

    server.handle("GET", "/healthz", [&](usub::server::protocols::http::Request &request,
                                         usub::server::protocols::http::Response &response
          ) -> usub::uvent::task::Awaitable<void> {
                      response.setStatus(200).addHeader("Content-Type", "application/json").setBody(
                          R"({"status": true})");
                      co_return;
                  });

    server.handle("GET", "/startup", startup_probe);
    server
    .handle("POST", R"(/api/user/create)",
            bind_handler<&article::handler::UserHandler::createUser>(user_handler));
    server
    .handle("POST", R"(/api/user/update)",
            bind_handler<&article::handler::UserHandler::updateUser>(user_handler));
    server
    .handle("POST", R"(/api/user/load)",
            bind_handler<&article::handler::UserHandler::loadUser>(user_handler));

    server.run();

    return 0;
}
```

That's it. Now just follow steps below.

1. Need to start up KeyDB and PostgreSQL
```bash
docker compose up -d
```
2. Need to start up server (from project folder):
```bash
(cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) && ./article) 
```


Create user test:
```bash
curl --location 'http://127.0.0.1:17000/api/user/create' \
--header 'Content-Type: application/json' \
--data '{
    "name": "John",
    "password": "qwerty",
    "roles": [
        "User", "Admin"
    ]
}'
```
Update user test:
```bash
curl --location 'http://127.0.0.1:17000/api/user/update' \
--header 'Content-Type: application/json' \
--data '{
    "id": "1",
    "name": "Johnny",
    "old_password": "qwerty",
    "new_password": "qwerty1",
    "roles": ["Admin"]
}'
```
Load user test:
```bash
curl --location 'http://127.0.0.1:17000/api/user/load' \
--header 'Content-Type: application/json' \
--data '{
    "id": "1"
}'
```

Full code can be found in that [repository](https://github.com/Usub-development/introduction).

---
This article provides representative example working with our technology stack.
However, there are many useful features for production which weren't included in that article such
as [channels](https://usub-development.github.io/uvent/channels/channels/) (similar
to [Go](https://go.dev) channels),
[delayed coroutines calls](https://github.com/Usub-development/uvent/blob/e2f2931e71fa6973d57a0f0a50174b9ab790f833/examples/main_timers.cpp#L61)
from `uvent` (and many others), native JSON support in `upq` etc. You can find more details by
followed links:

- [uvent](https://github.com/Usub-development/uvent)
- [unet](https://github.com/Usub-development/unet)
- [ureflect](https://github.com/Usub-development/ureflect)
- [upq](https://github.com/Usub-development/upq)
- [uredis](https://github.com/Usub-development/uredis)
- [ulog](https://github.com/Usub-development/ulog)
- [ujson](https://github.com/Usub-development/ujson)