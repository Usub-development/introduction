//
// Created by kirill on 12/27/25.
//

#ifndef ARTICLE_REQUEST_H
#define ARTICLE_REQUEST_H

#include <string>
#include <cstdint>

namespace article::dto {
   struct CreateArticle {
      int author_id;
      std::string title;
      std::string body;
      int status;
   };
}

#endif //ARTICLE_REQUEST_H
