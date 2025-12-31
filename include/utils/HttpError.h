//
// Created by kirill on 12/31/25.
//

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
