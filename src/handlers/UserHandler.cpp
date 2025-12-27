//
// Created by kirill on 12/27/25.
//

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
