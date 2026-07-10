/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "uri.h"

#include <cctype>
#include <iomanip>

#include "global/stringutils.h"

#include "log.h"

using namespace muse;

static const std::string URI_VAL_TRUE("true");
static const std::string URI_VAL_FALSE("false");

static bool isUnreservedChar(char c)
{
    static const std::string unreserved
        ="abcdefghijklmnopqrstuvwxyz"
         "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
         "0123456789"
         "-_.~";

    return unreserved.find(c) != std::string::npos;
}

std::string Uri::percentEncode(const std::string& str, const std::string& extraSafeChars)
{
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex << std::uppercase;

    for (unsigned char c : str) {
        if (isUnreservedChar(static_cast<char>(c)) || extraSafeChars.find(static_cast<char>(c)) != std::string::npos) {
            encoded << static_cast<char>(c);
        } else {
            encoded << '%' << std::setw(2) << static_cast<int>(c);
        }
    }

    return encoded.str();
}

//! NOTE Sequences that aren't valid %XX escapes are left untouched, so freeform text
//! containing a literal '%' round-trips safely.
std::string Uri::percentDecode(const std::string& str)
{
    std::string decoded;
    decoded.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i) {
        bool isEscape = str[i] == '%' && i + 2 < str.size()
                        && std::isxdigit(static_cast<unsigned char>(str[i + 1]))
                        && std::isxdigit(static_cast<unsigned char>(str[i + 2]));

        if (isEscape) {
            decoded += static_cast<char>(std::stoi(str.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded += str[i];
        }
    }

    return decoded;
}

// muse://module/target/name

static std::string extractPath(const std::string& str)
{
    auto schemePos = str.find(':');
    auto paramsPos = str.find('?');
    auto pathPos= (schemePos != std::string::npos) ? (schemePos + 3) : 0;
    size_t pathN = (paramsPos != std::string::npos) ? (paramsPos - pathPos) : std::string::npos;

    return str.substr(pathPos, pathN);
}

Uri::Uri(const std::string& str)
{
    auto schemePos = str.find(':');
    if (schemePos != std::string::npos) {
        m_scheme = str.substr(0, schemePos);
    }

    m_path = extractPath(str);
}

Uri::Uri(const String& str)
    : Uri(str.toStdString())
{
}

Uri::Uri(const std::string& scheme, const std::string& path)
    : m_scheme(scheme)
{
    m_path = extractPath(path);
}

bool Uri::isValid() const
{
    if (m_scheme.empty()) {
        return false;
    }

    if (m_path.empty()) {
        return false;
    }

    return true;
}

const Uri::Scheme& Uri::scheme() const
{
    return m_scheme;
}

void Uri::setScheme(const Scheme& scheme)
{
    m_scheme = scheme;
}

const std::string& Uri::path() const
{
    return m_path;
}

void Uri::setPath(const std::string& path)
{
    m_path = path;
}

std::string Uri::toString() const
{
    return m_scheme + "://" + m_path;
}

Uri Uri::fromLocalFile(const io::path_t& path)
{
    std::string absolutePath = io::absoluteFilePath(path).toStdString();

    std::replace(absolutePath.begin(), absolutePath.end(), '\\', '/');

    return Uri("file", percentEncode(absolutePath, "/"));
}

io::path_t Uri::toLocalFile() const
{
    return io::path_t(percentDecode(m_path));
}

// muse://module/target/name?param1=value1&paramn=valuen

UriQuery::UriQuery(const std::string& str)
    : m_uri(str)
{
    parseParams(str, m_params);
}

UriQuery::UriQuery(const String& str)
    : UriQuery(str.toStdString())
{
}

UriQuery::UriQuery(const Uri& uri)
    : m_uri(uri)
{
}

void UriQuery::parseParams(const std::string& uri, Params& out) const
{
    auto paramsPos = uri.find('?');
    if (paramsPos == std::string::npos) {
        return;
    }

    std::string paramsStr = uri.substr(paramsPos + 1);

    strings::trim(paramsStr);

    std::map<std::string, std::string> placeholders;
    std::vector<std::string> quotesStrings;
    extractQuotedStrings(paramsStr, quotesStrings);
    for (size_t i = 0; i < quotesStrings.size(); ++i) {
        std::string key = "s" + std::to_string(i);
        const std::string& val = quotesStrings.at(i);

        strings::replace(paramsStr, val, key);
        placeholders[key] = val;
    }

    std::vector<std::string> paramsPairs;
    strings::split(paramsStr, paramsPairs, "&");

    for (const std::string& pair : paramsPairs) {
        std::vector<std::string> param;
        strings::split(pair, param, "=");
        if (param.size() != 2) {
            LOGE() << "Invalid param: " << pair << ", in uri: " << uri;
            continue;
        }
        std::string key = param.at(0);
        strings::trim(key);

        std::string val = param.at(1);

        //! NOTE Val is bool?
        if (URI_VAL_TRUE == val || URI_VAL_FALSE == val) {
            out[key] = Val(val == URI_VAL_TRUE);
            continue;
        }

        auto it = placeholders.find(val);
        if (it != placeholders.end()) {
            val = it->second;
        }

        strings::trim(val);

        if (val.size() > 2 && val.at(0) == '\'' && val.at(val.size() - 1) == '\'') {
            val = val.substr(1, val.size() - 2);
        }

        out[key] = Val(Uri::percentDecode(val));
    }
}

void UriQuery::extractQuotedStrings(const std::string& str, std::vector<std::string>& out) const
{
    //! NOTE It is necessary to get substrings limited to single quotes from a string
    //! Example - "path='path/to/file.jpg'"

    int bi = -1;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str.at(i) == u'\'') {
            if (bi == -1) { // begin quotes string
                bi = int(i);
            } else {  // end quotes string
                out.push_back(str.substr(bi, i - bi + 1));
                bi = -1;
            }
        }
    }
}

std::string UriQuery::toString() const
{
    std::string str = m_uri.toString();
    if (!m_params.empty()) {
        str += "?";
        for (auto it = m_params.cbegin(); it != m_params.cend(); ++it) {
            std::string valStr = it->second.toString();
            if (it->second.type() == Val::Type::String) {
                valStr = Uri::percentEncode(valStr);
            }

            str += it->first + "=" + valStr + "&";
        }

        str.erase(str.size() - 1);
    }
    return str;
}

bool UriQuery::isValid() const
{
    return m_uri.isValid();
}

const Uri& UriQuery::uri() const
{
    return m_uri;
}

void UriQuery::setScheme(const Uri::Scheme& scheme)
{
    m_uri.setScheme(scheme);
}

const UriQuery::Params& UriQuery::params() const
{
    return m_params;
}

Val UriQuery::param(const std::string& key, const Val& def) const
{
    auto it = m_params.find(key);
    if (it == m_params.end()) {
        return def;
    }
    return it->second;
}

void UriQuery::addParam(const std::string& key, const Val& val)
{
    m_params[key] = val;
}

UriQuery& UriQuery::set(const ValMap& vals)
{
    m_params = vals;
    return *this;
}

UriQuery& UriQuery::set(const std::string& key, const Val& val)
{
    m_params[key] = val;
    return *this;
}

UriQuery UriQuery::addingParam(const std::string& key, const Val& val) const
{
    UriQuery copy(*this);
    copy.addParam(key, val);
    return copy;
}

bool UriQuery::contains(const std::string& key) const
{
    return m_params.count(key) > 0;
}

bool UriQuery::operator==(const UriQuery& query) const
{
    return m_uri == query.m_uri && m_params == query.m_params;
}

bool UriQuery::operator!=(const UriQuery& query) const
{
    return !(*this == query);
}
