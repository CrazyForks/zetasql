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

#ifndef GOOGLESQL_PUBLIC_PARSE_TOKENS_H_
#define GOOGLESQL_PUBLIC_PARSE_TOKENS_H_

#include <string>
#include <vector>

#include "googlesql/public/language_options.h"
#include "googlesql/public/parse_location.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"

namespace googlesql {

/*
ParseToken represents one element in a statement fragment tokenized by
GetParseTokens. This is an abstraction over the tokens as they would be seen by
the GoogleSQL parser.

The token will be valid - if a token results in an error during processing,
GetParseTokens produces an error Status, and the invalid token is discarded. The
token will have quoting and escaping removed, and literals will be constructed
into concrete googlesql::Values.

Each token is one of the following:
  * Keyword: Definitely a keyword (or symbol)
    - Is one of: (A) in the list of GoogleSQL keywords (either reserved or
      non-reserved), (B) is in a hard-coded list to be treated as keywords for
      historical reasons (e.g. "current_date"), or (C) is not one of the other
      token types handled by this API (e.g. symbols).
    - IsKeyword() = true, IsIdentifier() = false.
    - IMPORTANT: Some keywords can be treated as identifiers based on context.
      Callers need to account for this in order to get consistent results from
      this API as new keywords are added to the language. See the note below for
      details.
  * Identifier: Definitely an identifier
    - Is known to be an identifier, and not a keyword. This either means that
      the identifier is quoted with backticks (e.g. "`ident`") or contains
      characters which are not valid in a keyword (e.g. "12" in "SELECT x.12").
    - IsKeyword() = false, IsIdentifier() = true.
  * Keyword or identifier: May be either
    - Could be a keyword or an identifier, depending on context.
    - IsKeyword() = true, IsIdentifier() = true.
    - IMPORTANT: These tokens may become Keyword-type tokens when new keywords
      are added to the language. Callers need to account for this in order to
      get consistent results from this API. See the note below for details.
  * Literal value
    - A simple literal, such as an integer, floating point, string, or bytes.
    - Not all GoogleSQL literals will produce this token type, as some literals
      are defined in the parser grammar. For example the DATE-type literal in
      `SELECT DATE "2026-04-01"` would produce a series of tokens, including a
      string literal.
    - IsValue() = true.
  * END_OF_INPUT
    - The last token, if the whole input was processed successfully.
    - IsEndOfInput() = true.

--

IMPORTANT!: Callers must handle keywords when identifiers are expected!

Because this API produces tokens without using the GoogleSQL parser grammar, it
it cannot distinguish cases when a keyword is used in a manner where it would be
treated as an identifier based on context. Callers MUST handle this explicitly
in order to get consistent results that do not change when new keywords are
added to GoogleSQL!

For example, given the SQL `SELECT a.b.SOMEVAL.x.y`, the fragment `SOMEVAL`
would be classified as "Keyword or identifier" if "SOMEVAL" is not in the list
of GoogleSQL keywords, but would be classified as "Keyword" if "SOMEVAL" is
later added to the list of GoogleSQL keywords.

In practice, this means that in most cases where a caller uses IsIdentifier()
and GetIdentifier() to get the text of an identifier, they must ALSO use
IsKeyword() and GetImage() to get the text, in case that identifier is added as
a keyword in the future. See (broken link) for a practical example fixing such an
issue, where the caller assumed that only GetIdentifier() was required.

--

For example, the table name to drop could be approximately extracted from a
statement like "DROP TABLE <table_name>;" as follows:

  // Enable options relevant to your context.
  googlesqL::LanguageOptions language_options;
  language_options.EnableMaximumLanguageFeatures();

  googlesql::ParseTokenOptions parse_token_options = {
      .stop_at_end_of_statement = true,
      .language_options = language_options};
  auto resume_location = googlesql::ParseResumeLocation::FromStringView(sql);
  std::vector<googlesql::ParseToken> parse_tokens;
  GOOGLESQL_RETURN_IF_ERROR(googlesql::GetParseTokens(
      parse_token_options, &resume_location, &parse_tokens));

  if (parse_tokens.size() < 4 || parse_tokens[0].GetKeyword() != "DROP" ||
      parse_tokens[1].GetKeyword() != "TABLE") {
      // Should have at least DROP, TABLE, one name part, and semicolon or EOI.
      return <error>;
  }

  // Should be alternating name part and dot, until we reach semicolon or EOI.
  std::vector<std::string> name_parts;
  for (int i = 2; i < parse_tokens.size(); i++) {
    // Expect a name part.
    if (parse_tokens[i].IsIdentifier()) {
      name_parts.push_back(parse_tokens[i].GetIdentifier());
    } else if (parse_tokens[i].IsKeyword()) {
      name_parts.push_back(std::string(parse_tokens[i].GetImage()));
    } else {
      // Table name is not correct in SQL text.
      return <error>;
    }

    // After name part can be dot, semicolon, or EOI.
    i++;
    GOOGLESQL_RET_CHECK(i < parse_tokens.size());  // Should end with semicolon or EOI.
    if (parse_tokens[i].IsKeyword() && parse_tokens[i].GetKeyword() == ".") {
      continue;  // Keep processing following name parts.
    } else if (
        (parse_tokens[i].IsKeyword() && parse_tokens[i].GetKeyword() == ";") ||
        parse_tokens[i].IsEndOfInput) {
      break;  // Done the statement.
    } else {
      // Table name is not correct in SQL text.
      return <error>;
    }
  }

Caveats with this example:
  - Real GoogleSQL `DROP TABLE` statements accept more syntax, such as `IF
    EXISTS`. The more complicated the parser grammar, the harder it is to use
    this API to re-create and accept the syntax accepted by the parser.
  - This API doesn't have any way to guarantee that a keyword token would parse
    as a valid part in a table name, for example the token could be a reserved
    keyword at the start of the table name path, or it could be a symbol which
    is grouped with keywords by this API. This example will add any keyword to
    the table name path.

*/
class ParseToken {
 public:
  bool IsEndOfInput() const { return kind_ == END_OF_INPUT; }

  // True if this token can be treated as a keyword.
  bool IsKeyword() const {
    return kind_ == KEYWORD || kind_ == IDENTIFIER_OR_KEYWORD;
  }
  // True if this token can be treated as an identifier.
  bool IsIdentifier() const {
    return kind_ == IDENTIFIER || kind_ == IDENTIFIER_OR_KEYWORD;
  }
  // True if this token is a value.
  bool IsValue() const { return kind_ == VALUE; }
  // True if this token is a comment.
  bool IsComment() const { return kind_ == COMMENT; }

  // True if there is no whitespace between this token and the previous token.
  bool IsAdjacentToPreviousToken() const { return adjacent_to_prior_token_; }

  // Get the keyword, or "" if the token is not a keyword. Returns the keyword
  // in upper case.
  std::string GetKeyword() const;

  // Get the identifier, or "" if the token is not an identifier. Returns the
  // identifier with the original case, with quotes and escaping resolved.
  std::string GetIdentifier() const;

  // Returns the exact SQL string that was the input for this token.
  absl::string_view GetImage() const;

  // Get the Value for a token of kind VALUE.  Return invalid value for
  // other token types.
  //
  // Values are returned for literals of types STRING, BYTES, INT64,
  // UINT64 or DOUBLE only.
  // NULL is returned as a keyword, so the Value is never NULL.
  // TRUE/FALSE are returned as keywords, so the Value never has type BOOL.
  // Other complex multi-token literals like DATE "2011-02-03" and array
  // or struct construction literals will be returned as token sequences.
  // Negative numeric literals will be returned as a "-" token followed by
  // an integer or double value literal.
  Value GetValue() const;

  // Get a SQL string for this token.  This includes quoting and escaping as
  // necessary, but is not necessarily what was included in the original input.
  // Concatenating GetSQL() strings together (with spaces) will give a statement
  // that can be parsed back to the same tokens.
  std::string GetSQL() const;

  // Get a descriptive string for this token that includes the token kind and
  // the value.
  std::string DebugString() const;

  // Returns the location of the token in the input.
  ParseLocationRange GetLocationRange() const { return location_range_; }

  // The declarations below are intended for internal use.

  enum Kind {
    KEYWORD,                // A googlesql keyword or symbol.
    IDENTIFIER,             // An identifier that was quoted.
    IDENTIFIER_OR_KEYWORD,  // An unquoted identifier.
    VALUE,                  // A literal value.
    COMMENT,                // A comment.
    END_OF_INPUT,           // The end of the input string was reached.
  };

  Kind kind() const { return kind_; }

  // The constructors are generally only called internally.
  ParseToken();

  // <image> and <value> are passed by value so they can be moved into place.
  ParseToken(ParseLocationRange location_range, bool adjacent_to_prior_token,
             std::string image, Kind kind);
  ParseToken(ParseLocationRange location_range, bool adjacent_to_prior_token,
             std::string image, Kind kind, Value value);

 private:
  Kind kind_;
  bool adjacent_to_prior_token_;
  std::string image_;
  ParseLocationRange location_range_;
  Value value_;

  // Copyable
};

struct ParseTokenOptions {
  // Return at most this many tokens (only if positive). It is not possible to
  // resume a GetParseTokens() call for which max_tokens was set.
  int max_tokens = 0;

  // Stop parsing after a ";" token.  The last token returned will be either
  // a ";" or an EOF.
  bool stop_at_end_of_statement = false;

  // Return the comments in the ParseToken vector or silently drop them.
  bool include_comments = false;

  LanguageOptions language_options;
};

// Gets a vector of ParseTokens starting from `resume_location`, and updates
// `resume_location` to point at the location after the tokens that were
// parsed. This is used to tokenize a string, following GoogleSQL tokenization
// rules for comments, quoting, literals, etc. Returns an error on any
// tokenization failure, like bad characters, unclosed quotes, invalid escapes,
// etc.
//
// This API cannot be used to perfectly mirror the GoogleSQL parser for most
// types of statements, and at best can be used as an approximation. For
// example, in a fragment like `SELECT x.y`, the GoogleSQL parser allows `x` to
// be a non-reserved keyword but not a reserved keyword, and allows `y` to be
// either. This API doesn't distinguish reserved and non-reserved keywords, and
// also doesn't distinguish non-keyword things (e.g. symbols) which are grouped
// with keywords by this API.
//
// It's strongly recommended to prefer other, more appropriate, public APIs
// (such as the Analyzer or parse_helpers.cc) to reliably extract details like
// statement validity, statement type, and meaning.
//
// By default, if OK status is returned, the entire string will be consumed and
// converted to ParseTokens, the output will have at least one token, and the
// last token will always have IsEndOfInput()=true.
//
// With some `options`, the entire input string may not be consumed in one
// call. In those cases, GetParseTokens can be called in a loop using a
// ParseResumeLocation to continue where it left off. The loop can stop once one
// iteration returns a vector ending with an IsEndOfInput() token. This usage
// can be mixed with AnalyzeNextStatement and GetNextStatementKind. Only calls
// to GetParseTokens that parse an entire statement can be resumed; i.e., a call
// to with option `max_tokens` cannot be resumed.
//
// Returned Statuses may be annotated with an ErrorLocation payload to indicate
// where an error occurred.
//
// See the comment on ParseToken for interpretation of the ParseToken types.
absl::Status GetParseTokens(const ParseTokenOptions& options,
                            ParseResumeLocation* resume_location,
                            std::vector<ParseToken>* tokens);

}  // namespace googlesql

#endif  // GOOGLESQL_PUBLIC_PARSE_TOKENS_H_
