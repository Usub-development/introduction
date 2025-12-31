//
// Created by kirill on 12/27/25.
//

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