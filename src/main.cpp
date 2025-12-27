//
// Created by kirill on 12/24/25.
//

#include <server/server.h>
#include <upq/PgRouting.h>
#include <upq/PgRoutingBuilder.h>
#include <ulog/ulog.h>
#include <uredis/RedisClusterClient.h>

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
    } {
        auto r = co_await pool->query_awaitable(R"SQL(
        CREATE TABLE IF NOT EXISTS public.articles (
          id BIGSERIAL PRIMARY KEY,
          author_id BIGINT NOT NULL REFERENCES public.users(id) ON DELETE RESTRICT,
          title TEXT NOT NULL,
          body TEXT NOT NULL,
          status SMALLINT NOT NULL DEFAULT 0, -- 0=draft, 1=published, 2=archived
          likes BIGINT NOT NULL DEFAULT 0,
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

    usub::uredis::RedisClusterConfig uredis_cfg;
    uredis_cfg.seeds = {{"127.0.0.1", 6379}};

    usub::uredis::RedisClusterClient uredis_client{uredis_cfg};


    server.handle("GET", "/healthz", [&](usub::server::protocols::http::Request &request,
                                         usub::server::protocols::http::Response &response
          ) -> usub::uvent::task::Awaitable<void> {
                      response.setStatus(200).addHeader("Content-Type", "application/json").setBody(
                          R"({"status": true})");
                      co_return;
                  });

    server.handle("GET", "/startup", startup_probe);

    return 0;
}
