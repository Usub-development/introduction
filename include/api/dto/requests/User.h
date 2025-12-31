//
// Created by kirill on 12/27/25.
//

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
