//
// Created by kirill on 12/27/25.
//

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
}
