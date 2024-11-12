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

#include "zetasql/public/error_helpers.h"

#include <ctype.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zetasql/common/status_payload_utils.h"
#include "zetasql/common/utf_util.h"
#include "zetasql/proto/internal_error_location.pb.h"
#include "zetasql/proto/script_exception.pb.h"
#include "zetasql/public/deprecation_warning.pb.h"
#include "zetasql/public/error_location.pb.h"
#include "zetasql/public/options.pb.h"
#include "zetasql/public/parse_location.h"
#include "zetasql/base/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/cord.h"  
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "re2/re2.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_builder.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {

// Format an ErrorLocation using <format>, which is a string as in
// strings::Substitute, with $0 being file, $1 being line, and $2 being column.
// ErrorSource information is ignored (if present).
static std::string FormatErrorLocation(const ErrorLocation& location,
                                       const absl::string_view format) {
  // `input_start_column_offset` is only relevant when `line` is 1.
  // After the first line, the text starts from the first column anyway.
  int effective_column_offset =
      location.line() == 1 ? location.input_start_column_offset() : 0;
  return absl::Substitute(format, location.filename(),
                          location.line() + location.input_start_line_offset(),
                          location.column() + effective_column_offset);
}

std::string FormatErrorLocation(const ErrorLocation& location) {
  return (location.has_filename() ? FormatErrorLocation(location, "$0:$1:$2")
                                  : FormatErrorLocation(location, "$1:$2"));
}

// Internal helper function to format the ErrorLocation string with format
// [at file:line:column]
static std::string FormatErrorLocationAtFileLineColumn(
    const ErrorLocation& location) {
  return absl::StrCat("[at ", FormatErrorLocation(location), "]");
}

std::string FormatErrorLocation(const ErrorLocation& location,
                                absl::string_view input_text,
                                ErrorMessageMode mode) {
  std::string error_location_string =
      FormatErrorLocationAtFileLineColumn(location);
  if (mode == ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET) {
    absl::StrAppend(&error_location_string, "\n",
                    GetErrorStringWithCaret(input_text, location));
  }

  if (!location.error_source().empty()) {
    const std::string error_source_separator =
        (mode == ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET ? "\n"
                                                                       : "; ");
    std::string error_source_string;
    for (const ErrorSource& error_source : location.error_source()) {
      const std::string source_message = FormatErrorSource(error_source, mode);
      error_source_string = absl::StrCat(
          source_message,
          (!error_source_string.empty() ? error_source_separator : ""),
          error_source_string);
    }
    absl::StrAppend(
        &error_location_string,
        (!error_source_string.empty() ? error_source_separator : ""),
        error_source_string);
  }
  return error_location_string;
}

std::string FormatErrorSource(const ErrorSource& error_source,
                              ErrorMessageMode mode) {
  if (mode == ErrorMessageMode::ERROR_MESSAGE_WITH_PAYLOAD) {
    return "";
  }
  std::string message = error_source.error_message();
  if (!message.empty() && error_source.has_error_location()) {
    // Note - if error_source.error_location has an ErrorSource, it is ignored.
    absl::StrAppend(
        &message, " ",
        FormatErrorLocationAtFileLineColumn(error_source.error_location()));
  }
  if (mode == ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET &&
      error_source.has_error_message_caret_string()) {
    absl::StrAppend(&message, (!message.empty() ? "\n" : ""),
                    error_source.error_message_caret_string());
  }
  return message;
}

std::string FormatError(const absl::Status& status) {
  if (status.code() != absl::StatusCode::kInvalidArgument) {
    absl::Status stripped_status = status;
    stripped_status.ErasePayload(kErrorMessageModeUrl);
    return internal::StatusToString(stripped_status);
  }

  std::string message = std::string(status.message());
  if (internal::HasPayload(status)) {
    std::string payload_string;
    std::string location_string;
    absl::Status stripped_status = status;

    if (internal::HasPayloadWithType<ErrorLocation>(stripped_status)) {
      // Perform special formatting for location data.
      ErrorLocation location =
          internal::GetPayload<ErrorLocation>(stripped_status);

      location_string = absl::StrCat(
          " ", FormatErrorLocation(location, /*input_text=*/"",
                                   ErrorMessageMode::ERROR_MESSAGE_ONE_LINE));

      internal::ErasePayloadTyped<ErrorLocation>(&stripped_status);
    }
    stripped_status.ErasePayload(kErrorMessageModeUrl);
    payload_string = internal::PayloadToString(stripped_status);

    // Error messages with a caret look strange if the payload immediately
    // follows the caret, so put it on separate line in that case, being careful
    // to avoid turning a single-line error message into a multi-line one.
    absl::string_view payload_separator;
    if (!payload_string.empty()) {
      const bool multiline = absl::StrContains(message, '\n');
      payload_separator = multiline ? "\n" : " ";
    }
    absl::StrAppend(&message, location_string, payload_separator,
                    payload_string);
  }
  return message;
}

bool HasErrorLocation(const absl::Status& status) {
  return internal::HasPayloadWithType<ErrorLocation>(status);
}

bool GetErrorLocation(const absl::Status& status, ErrorLocation* location) {
  if (HasErrorLocation(status)) {
    *location = internal::GetPayload<ErrorLocation>(status);
    return true;
  }
  return false;
}

void ClearErrorLocation(absl::Status* status) {
  return internal::ErasePayloadTyped<ErrorLocation>(status);
}

static bool IsWordChar(char c) { return isalnum(c) || c == '_'; }

// Return true if <column> (0-based) in <str> starts a word.
static bool IsWordStart(absl::string_view str, int column) {
  ABSL_DCHECK_LT(column, str.size());
  if (column == 0 || column >= str.size()) return true;
  return !IsWordChar(str[column - 1]) && IsWordChar(str[column]);
}

// Constructs and returns a truncated input string based on <input>, <location>,
// and <max_width_in>.  Also returns the error column.
static void GetTruncatedInputStringInfo(absl::string_view input,
                                        const ErrorLocation& location,
                                        int max_width_in,
                                        std::string* truncated_input,
                                        int* error_column) {
  // We don't allow a max_width below a certain size.
  constexpr int kMinimumMaxWidth = 30;
  // If the error line is longer than max_width, give a substring of up
  // to max_width characters, with the caret near the middle of it.
  // We need some minimum width.
  const int max_width = std::max(max_width_in, kMinimumMaxWidth);

  ABSL_DCHECK_GT(location.line(), 0);
  ABSL_DCHECK_GT(location.column(), 0);

  ParseLocationTranslator translator(input);
  absl::StatusOr<absl::string_view> line_text =
      translator.GetLineText(location.line());
  ZETASQL_DCHECK_OK(line_text.status());

  *truncated_input = translator.ExpandTabs(line_text.value_or(""));

  // location.column() may be one off the end of the line for EOF errors.
  ABSL_DCHECK_LE(location.column(), truncated_input->size() + 1);
  // error_column is 0-based.
  *error_column =
      std::max(1, std::min(static_cast<int>(truncated_input->size() + 1),
                           location.column())) -
      1;

  // `input_start_column_offset` is only relevant when `line` is 1.
  // After the first line, the text starts from the first column anyway.
  int effective_column_offset =
      location.line() == 1 ? location.input_start_column_offset() : 0;
  if (effective_column_offset + truncated_input->size() > max_width) {
    const int one_half = max_width / 2;
    const int one_third = max_width / 3;
    // If the error is near the start, just use a prefix of the string.
    if (*error_column + effective_column_offset > max_width - one_third) {
      // Otherwise, try to find a word boundary to start the string on
      // that puts the caret in the middle third of the output line.
      int found_start = -1;
      for (int start_column = std::max(0, *error_column - 2 * one_third);
           start_column < std::max(0, *error_column - one_third);
           ++start_column) {
        if (IsWordStart(*truncated_input, start_column)) {
          found_start = start_column;
          break;
        }
      }
      if (found_start == -1) {
        // Didn't find a good separator.  Just split in the middle.
        found_start = std::max(*error_column - one_half, 0);
      }

      // Add ... prefix if necessary.
      if (effective_column_offset + found_start < 3) {
        found_start = 0;
      } else {
        *truncated_input =
            absl::StrCat("...", truncated_input->substr(found_start));
        *error_column -= found_start - 3;
      }
    }
    *truncated_input = PrettyTruncateUTF8(*truncated_input, max_width);
    ABSL_DCHECK_LE(*error_column, truncated_input->size());
  }
}

// Helper function to return an error string from an error line and column.
static std::string GetErrorStringFromErrorLineAndColumn(
    absl::string_view error_line, const int error_column) {
  return absl::StrFormat("%s\n%*s^", error_line, error_column, "");
}

std::string GetErrorStringWithCaret(absl::string_view input,
                                    const ErrorLocation& location,
                                    int max_width_in) {
  std::string error_line;
  int error_column;
  GetTruncatedInputStringInfo(input, location, max_width_in, &error_line,
                              &error_column);
  return GetErrorStringFromErrorLineAndColumn(error_line, error_column);
}

// Returns a new status with all the payloads, but with the given message.
static absl::Status UpdateMessage(const absl::Status& status,
                                  absl::string_view message) {
  absl::Status new_status =
      absl::Status(status.code(), message);
  // Copy payloads
  status.ForEachPayload([&new_status](
      absl::string_view type_url, const absl::Cord& payload) {
    new_status.SetPayload(type_url, payload);});
  return new_status;
}

// Updates the `status` error string based on `input_text` and `mode`.
// See header comment for MaybeUpdateErrorFromPayload for details.
static absl::Status UpdateErrorFromPayload(absl::Status status,
                                           absl::string_view input_text,
                                           ErrorMessageMode mode,
                                           bool keep_error_location_payload) {
  if (status.ok()) {
    return status;
  }

  // Some callers want the payload to stay even as location is added to the
  // message, as indicated by 'keep_error_location_payload'. Callers also
  // expect this function to be idempotent, so multiple calls shout NOT result
  // in a status message looking like:
  //   "INVALID_ARGUMENT: unexpected END [at 1:35] [at 1:35] [at 1:35]"
  // This problem became possible thanks to the ability of keeping the payload
  // after updating the message, so now more information is needed to mark
  // already-processed statuses.
  // To accomplish this, this function tags any status the first time it sees
  // it. Subsequent calls check the tag to make sure the mode is the same, and
  // become a NOOP. This is to avoid adding the location information multiple
  // times.
  std::optional<absl::Cord> applied_mode_payload =
      status.GetPayload(kErrorMessageModeUrl);
  if (applied_mode_payload.has_value()) {
    ErrorMessageModeForPayload mode_already_applied;
    mode_already_applied.ParseFromCord(applied_mode_payload.value());
    ZETASQL_RET_CHECK_EQ(mode_already_applied.mode(), mode);
    return status;
  }

  if (mode == ErrorMessageMode::ERROR_MESSAGE_WITH_PAYLOAD) {
    // In this case, we do not update the error message and the payload
    // remains on the Status.
    return status;
  }

  if (internal::HasPayloadWithType<InternalErrorLocation>(status)) {
    // The error location is "internal", which means that it comes directly
    // from the AST or ResolvedAST parse locations. We must first convert it
    // to an ErrorLocation, which converts byte offsets to column number
    // and line number. We do not return internal error locations, but customers
    // may attach internal locations to errors e.g. during algebrization to
    // allow users to trace algebra errors to SQL sources.
    status = ConvertInternalErrorLocationToExternal(status, input_text);
  }

  ErrorLocation location;
  if (!GetErrorLocation(status, &location)) {
    return status;
  }

  std::string new_message = absl::StrCat(
      status.message(), " ", FormatErrorLocation(location, input_text, mode));
  // Update the message.  Leave everything else as is.
  absl::Status new_status = UpdateMessage(status, new_message);
  ErrorMessageModeForPayload mode_wrapper;
  mode_wrapper.set_mode(mode);
  new_status.SetPayload(kErrorMessageModeUrl, mode_wrapper.SerializeAsCord());
  if (!keep_error_location_payload) {
    ClearErrorLocation(&new_status);
  }
  return new_status;
}

// Remove ZetaSql-owned payloads from `status`. Returns true if all payloads
// are removed, or false if some non-ZetaSql-owned payloads remain.
static bool RedactZetaSqlOwnedPayloads(absl::Status& status) {
  status.ErasePayload(kErrorMessageModeUrl);
  internal::ErasePayloadTyped<ErrorLocation>(&status);
  internal::ErasePayloadTyped<DeprecationWarning>(&status);
  internal::ErasePayloadTyped<ScriptException>(&status);
  return !internal::HasPayload(status);
}

static std::string PartiallyRedactIdentifier(absl::string_view identifier) {
  // To improve stability, convert to uppercase and show only the last part of
  // a multi-part identifier.
  std::vector<absl::string_view> dot_parts = absl::StrSplit(identifier, '.');
  std::vector<absl::string_view> colon_parts =
      absl::StrSplit(dot_parts.back(), ':');
  return absl::AsciiStrToUpper(colon_parts.back());
}

static std::string GetRedactedErrorMessage(
    const absl::Status& status, bool enable_partial_error_redaction) {
  if (!enable_partial_error_redaction) {
    // Full redaction
    return "SQL ERROR";
  }
  std::string function_name;
  if (RE2::PartialMatch(
          status.message(),
          R"re(No\s+matching\s+signature\s+for\s+(?:(?:aggregate|analytic)\s+)?function ([A-Za-z0-9_\.\:]*))re",
          &function_name)) {
    return absl::Substitute("FUNCTION_SIGNATURE_MISMATCH: $0",
                            PartiallyRedactIdentifier(function_name));
  }
  if (absl::StrContains(status.message(), "FUNCTION_SIGNATURE_MISMATCH")) {
    // Sometimes, we are asked to redact a message that has already been
    // redacted; just return it as is.
    return std::string(status.message());
  }
  std::string message =
      absl::StrCat("Unable to redact unknown error: ", status.message());
  // In debug builds, crash if we see a message we don't know how to redact in
  // order to ensure that the string we return does not accidentally leak into
  // the expected output of engine tests, which would cause breakage if any
  // new error messages get supported here in the future.
  ABSL_DCHECK(false) << message;

  return message;
}

// Returns a status with the same payloads, but the message changed if needed
// to apply the given stability_mode. For example: TEST_REDACTED ensures the
// relevant errors are redacted, usually for file-based tests that depend on
// ZetaSQL but not about the exact message text.
static absl::Status ApplyErrorMessageStabilityMode(
    const absl::Status& status, ErrorMessageStability stability_mode,
    bool enable_partial_error_redaction) {
  if (status.ok()) {
    return status;
  }

  switch (stability_mode) {
    case ERROR_MESSAGE_STABILITY_UNSPECIFIED:
    case ERROR_MESSAGE_STABILITY_PRODUCTION:
      return status;
    case ERROR_MESSAGE_STABILITY_TEST_REDACTED_WITH_PAYLOADS:
      switch (status.code()) {
        case absl::StatusCode::kInvalidArgument:
        case absl::StatusCode::kAlreadyExists:
        case absl::StatusCode::kNotFound: {
          // Erase ZetaSQL payloads
          ZETASQL_RET_CHECK(
              !internal::HasPayloadWithType<InternalErrorLocation>(status));
          // Make a copy so that we don't remove the payloads from 'status'
          absl::Status for_payload_stripping = status;
          if (RedactZetaSqlOwnedPayloads(for_payload_stripping)) {
            // All of the payloads attached to 'status' are ZetaSQL-owned.
            // Redact the error message from the original status with all
            // payloads to return.
            return UpdateMessage(status,
                                 GetRedactedErrorMessage(
                                     status, enable_partial_error_redaction));
          }
          // There are still payloads after removing the ZetaSQL-owned ones,
          // so return the original status.
          return status;
        }
        default:
          return status;
      }
      break;
    case ERROR_MESSAGE_STABILITY_TEST_REDACTED:
      switch (status.code()) {
        case absl::StatusCode::kInvalidArgument:
        case absl::StatusCode::kAlreadyExists:
        case absl::StatusCode::kNotFound: {
          ZETASQL_RET_CHECK(
              !internal::HasPayloadWithType<InternalErrorLocation>(status));
          // Make a non-const copy.
          absl::Status redacted = status;
          if (RedactZetaSqlOwnedPayloads(redacted)) {
            // Redact the message only if there are no other payloads. If there
            // are, it is likely the message comes from a dependency outside of
            // ZetaSQL.
            redacted = UpdateMessage(
                redacted, GetRedactedErrorMessage(
                              status, enable_partial_error_redaction));
          }
          return redacted;
        }
        default:
          return status;
      }
      ZETASQL_RET_CHECK_FAIL() << "Must return from within the switch on status code.";
  }
  ZETASQL_RET_CHECK_FAIL() << "Must return from within the switch on mode";
}

absl::Status MaybeUpdateErrorFromPayload(ErrorMessageOptions options,
                                         absl::string_view input_text,
                                         const absl::Status& status) {
  if (status.ok()) {
    // We do not update the error string with error payload, which
    // could include location and/or nested errors.  We leave any payload
    // attached to the Status.
    return status;
  }

  return ApplyErrorMessageStabilityMode(
      UpdateErrorFromPayload(status, input_text, options.mode,
                             options.attach_error_location_payload),
      options.stability, options.enhanced_error_redaction);
}

absl::Status MaybeUpdateErrorFromPayload(ErrorMessageMode mode,
                                         bool keep_error_location_payload,
                                         absl::string_view input_text,
                                         const absl::Status& status) {
  return MaybeUpdateErrorFromPayload(
      ErrorMessageOptions{
          .mode = mode,
          .attach_error_location_payload = keep_error_location_payload},
      input_text, status);
}

absl::Status UpdateErrorLocationPayloadWithFilenameIfNotPresent(
    const absl::Status& status, absl::string_view filename) {
  ErrorLocation error_location;
  if (filename.empty() || !GetErrorLocation(status, &error_location)) {
    return status;
  }

  if (error_location.has_filename()) {
    return status;
  }
  // The error location does not have a filename, so use the specified
  // 'module_filename'.
  error_location.set_filename(filename);

  absl::Status copy = status;
  ClearErrorLocation(&copy);
  internal::AttachPayload(&copy, error_location);
  return copy;
}

absl::Status ConvertInternalErrorLocationToExternal(
    absl::Status status, absl::string_view query, int input_start_line_offset,
    int input_start_column_offset) {
  if (!internal::HasPayloadWithType<InternalErrorLocation>(status)) {
    // Nothing to do.
    return status;
  }
  const InternalErrorLocation internal_error_location =
      internal::GetPayload<InternalErrorLocation>(status);

  const ParseLocationPoint error_point =
      ParseLocationPoint::FromInternalErrorLocation(internal_error_location);

  ParseLocationTranslator location_translator(query);

  std::pair<int, int> line_and_column;
  ZETASQL_ASSIGN_OR_RETURN(
      line_and_column,
      location_translator.GetLineAndColumnAfterTabExpansion(error_point),
      _ << "Location " << error_point.GetString() << " from status \""
        << internal::StatusToString(status) << "\" not found in query:\n"
        << query);
  ErrorLocation error_location;
  if (internal_error_location.has_filename()) {
    error_location.set_filename(internal_error_location.filename());
  }
  error_location.set_line(line_and_column.first);
  error_location.set_column(line_and_column.second);
  error_location.set_input_start_line_offset(input_start_line_offset);
  error_location.set_input_start_column_offset(input_start_column_offset);

  // Copy ErrorSource information if present.
  *error_location.mutable_error_source() =
      internal_error_location.error_source();

  absl::Status copy = status;
  internal::ErasePayloadTyped<InternalErrorLocation>(&copy);
  internal::AttachPayload(&copy, error_location);
  return copy;
}

}  // namespace zetasql
