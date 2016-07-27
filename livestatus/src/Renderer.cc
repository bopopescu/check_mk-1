// +------------------------------------------------------------------+
// |             ____ _               _        __  __ _  __           |
// |            / ___| |__   ___  ___| | __   |  \/  | |/ /           |
// |           | |   | '_ \ / _ \/ __| |/ /   | |\/| | ' /            |
// |           | |___| | | |  __/ (__|   <    | |  | | . \            |
// |            \____|_| |_|\___|\___|_|\_\___|_|  |_|_|\_\           |
// |                                                                  |
// | Copyright Mathias Kettner 2014             mk@mathias-kettner.de |
// +------------------------------------------------------------------+
//
// This file is part of Check_MK.
// The official homepage is at http://mathias-kettner.de/check_mk.
//
// check_mk is free software;  you can redistribute it and/or modify it
// under the  terms of the  GNU General Public License  as published by
// the Free Software Foundation in version 2.  check_mk is  distributed
// in the hope that it will be useful, but WITHOUT ANY WARRANTY;  with-
// out even the implied warranty of  MERCHANTABILITY  or  FITNESS FOR A
// PARTICULAR PURPOSE. See the  GNU General Public License for more de-
// tails. You should have  received  a copy of the  GNU  General Public
// License along with GNU Make; see the file  COPYING.  If  not,  write
// to the Free Software Foundation, Inc., 51 Franklin St,  Fifth Floor,
// Boston, MA 02110-1301 USA.

#include "Renderer.h"
#include <stdio.h>
#include <cmath>
#include <ostream>
#include "Logger.h"
#include "RendererCSV.h"
#include "RendererJSON.h"
#include "RendererPython.h"
#include "data_encoding.h"

extern int g_data_encoding;
extern int g_debug_level;

using std::make_unique;
using std::size_t;
using std::string;
using std::to_string;
using std::unique_ptr;
using std::vector;

Renderer::Renderer(OutputBuffer *output,
                   OutputBuffer::ResponseHeader response_header,
                   bool do_keep_alive, string invalid_header_message,
                   int timezone_offset)
    : _output(output), _timezone_offset(timezone_offset) {
    _output->setResponseHeader(response_header);
    _output->setDoKeepalive(do_keep_alive);
    if (invalid_header_message != "") {
        _output->setError(OutputBuffer::ResponseCode::invalid_header,
                          invalid_header_message);
    }
}

Renderer::~Renderer() = default;

// static
unique_ptr<Renderer> Renderer::make(
    OutputFormat format, OutputBuffer *output,
    OutputBuffer::ResponseHeader response_header, bool do_keep_alive,
    std::string invalid_header_message, const CSVSeparators &separators,
    int timezone_offset) {
    switch (format) {
        case OutputFormat::csv:
            return make_unique<RendererCSV>(
                output, response_header, do_keep_alive, invalid_header_message,
                separators, timezone_offset);
            break;
        case OutputFormat::json:
            return make_unique<RendererJSON>(
                output, response_header, do_keep_alive, invalid_header_message,
                timezone_offset);
        case OutputFormat::python:
            return make_unique<RendererPython>(
                output, response_header, do_keep_alive, invalid_header_message,
                timezone_offset);
    }
    return nullptr;  // unreachable
}

void Renderer::setError(OutputBuffer::ResponseCode code,
                        const string &message) {
    _output->setError(code, message);
}

size_t Renderer::size() const { return _output->size(); }

void Renderer::add(const string &str) { _output->add(str); }

void Renderer::add(const vector<char> &value) { _output->add(value); }

void Renderer::output(double value) {
    if (std::isnan(value)) {
        output(Null());
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.10e", value);
        add(buf);
    }
}

void Renderer::outputUnicodeEscape(unsigned value) {
    char buf[8];
    snprintf(buf, sizeof(buf), "\\u%04x", value);
    add(buf);
}

void Renderer::outputCharsAsBlob(const vector<char> &value) {
    for (char ch : value) {
        if (0 <= ch && ch <= 31) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\%03o", ch);
            add(buf);
        } else if (ch == '"' || ch == '\\') {
            add("\\");
            add(string(1, ch));
        } else {
            add(string(1, ch));
        }
    }
}

namespace {
void invalidUTF8(const string &value) {
    if (g_debug_level >= 2) {
        Informational() << "Ignoring invalid UTF-8 sequence in string '"
                        << value << "'";
    }
}
}  // namespace

void Renderer::outputCharsAsString(const string &value) {
    const char *r = value.c_str();
    std::size_t len = value.size();
    while (len != 0) {
        // Always escape control characters
        if (0 <= *r && *r <= 31) {
            outputUnicodeEscape(static_cast<unsigned>(*r));
        }

        else if (*r == '"' || *r == '\\') {
            add("\\");
            add(string(r, 1));
        }

        // Output ASCII characters unencoded
        else if (*r >= 32) {
            add(string(r, 1));
        }

        // TODO(sp): We actually assume signed chars here!!!
        // interpret two-Byte UTF-8 sequences in mode 'utf8' and 'mixed'
        else if ((g_data_encoding == ENCODING_UTF8 ||
                  g_data_encoding == ENCODING_MIXED) &&
                 ((*r & 0xE0) == 0xC0)) {
            outputUnicodeEscape(((*r & 31) << 6) |
                                (*(r + 1) & 0x3F));  // 2 byte encoding
            r++;
            len--;
        }

        // interpret 3/4-Byte UTF-8 sequences only in mode 'utf8'
        else if (g_data_encoding == ENCODING_UTF8) {
            // three-byte sequences (avoid buffer overflow!)
            if ((*r & 0xF0) == 0xE0) {
                if (len < 3) {
                    invalidUTF8(value);
                    break;  // end of string. No use in continuing
                } else {
                    outputUnicodeEscape(((*r & 0x0F) << 12 |
                                         (*(r + 1) & 0x3F) << 6 |
                                         (*(r + 2) & 0x3F)));
                    r += 2;
                    len -= 2;
                }
            }
            // four-byte sequences
            else if ((*r & 0xF8) == 0xF0) {
                if (len < 4) {
                    invalidUTF8(value);
                    break;  // end of string. No use in continuing
                } else {
                    outputUnicodeEscape(
                        ((*r & 0x07) << 18 | (*(r + 1) & 0x3F) << 6 |
                         (*(r + 2) & 0x3F) << 6 | (*(r + 3) & 0x3F)));
                    r += 3;
                    len -= 3;
                }
            } else {
                invalidUTF8(value);
            }
        }

        // in latin1 and mixed mode interpret all other non-ASCII characters as
        // latin1
        else {
            outputUnicodeEscape(static_cast<unsigned>(
                static_cast<int>(*r) + 256));  // assume latin1 encoding
        }

        r++;
        len--;
    }
}