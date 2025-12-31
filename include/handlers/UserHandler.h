//
// Created by kirill on 12/27/25.
//

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
