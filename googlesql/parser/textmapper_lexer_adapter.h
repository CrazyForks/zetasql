//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef GOOGLESQL_PARSER_TEXTMAPPER_LEXER_ADAPTER_H_
#define GOOGLESQL_PARSER_TEXTMAPPER_LEXER_ADAPTER_H_

#include <cstdint>

#include "googlesql/parser/tm_token.h"
#include "googlesql/parser/token_stream.h"
#include "googlesql/parser/token_with_location.h"
#include "googlesql/public/parse_location.h"
#include "absl/base/attributes.h"
#include "absl/strings/string_view.h"

namespace googlesql {
namespace parser {

// The TextMapperLexerAdapter adapts the token stream produced by GoogleSQL's
// intermediate components (macro expansion, lookahead transformer, etc) into
// a stream of tokens that can be consumed by the TextMapper parser.
class TextMapperLexerAdapter {
 public:
  ~TextMapperLexerAdapter() = default;

  explicit TextMapperLexerAdapter(TokenStream* input);

  TextMapperLexerAdapter(const TextMapperLexerAdapter& other) = delete;
  TextMapperLexerAdapter& operator=(const TextMapperLexerAdapter& other) =
      delete;
  TextMapperLexerAdapter(TextMapperLexerAdapter&& other) = delete;
  TextMapperLexerAdapter& operator=(TextMapperLexerAdapter&& other) = delete;

  ABSL_MUST_USE_RESULT Token Next();

  // Returns the 1-based line number of the last token returned by Next().
  ABSL_MUST_USE_RESULT int64_t Line() const {
    // TODO: Actually implement the logic. With the bison parser, we only
    // generate lines and columns when computing error location, and this
    // function always returned the static line number as it was unused in bison
    // locations, i.e. 1
    return 1;
  }

  // Returns the substring of the input corresponding to the last token.
  ABSL_MUST_USE_RESULT absl::string_view Text() const
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return last_token_.text;
  }

  // Returns the location of the last token.
  ABSL_MUST_USE_RESULT const ParseLocationRange& LastTokenLocation() const
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return last_token_.location;
  }

  // Returns the kind of the last token.
  ABSL_MUST_USE_RESULT Token Last() const { return last_token_.kind; }

 private:
  TokenStream* input_;
  TokenWithLocation last_token_;
};

}  // namespace parser

}  // namespace googlesql

#endif  // GOOGLESQL_PARSER_TEXTMAPPER_LEXER_ADAPTER_H_
