/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "CarbonEngine/Common.h"
#include "CarbonEngine/Game/MultiplayerSession.h"
#include <cstddef>
#include <string>

// Internal implementation shared by the native connection providers.
/** \cond CARBON_INTERNAL_SESSION_IMPLEMENTATION */
namespace Carbon
{
namespace SessionDetail
{

// Provider-independent manifest encoding and metadata validation. Keep the existing wire format
// and limits so extracting the protocol does not change Steam session compatibility.
class SessionManifestCodec
{
public:
    // Convert arbitrary player-name bytes to a delimiter-safe, canonical form.
    static std::string encodeManifestName(const std::string& name)
    {
        static const char digits[] = "0123456789ABCDEF";
        std::string result;
        for (const auto character : name)
        {
            const auto value = static_cast<unsigned char>(character);
            result += digits[value >> 4];
            result += digits[value & 15];
        }
        return result;
    }

    // Encode the complete metadata collection into the same canonical manifest as the roster.
    // Hex encoding preserves arbitrary value bytes without introducing ambiguous delimiters.
    static std::string encodeSessionMetadata(const MultiplayerSessionMetadata& metadata)
    {
        std::string encoded;
        for (const auto& entry : metadata)
        {
            if (!encoded.empty())
                encoded += ";";

            encoded += encodeManifestName(std::string(entry.first.cStr(), entry.first.length()));
            encoded += ":";
            encoded += encodeManifestName(std::string(entry.second.cStr(), entry.second.length()));
        }
        return encoded;
    }

    // Reject noncanonical or malformed data, and retain exact key/value bytes.
    static bool decodeSessionMetadata(const std::string& encoded, MultiplayerSessionMetadata& result)
    {
        MultiplayerSessionMetadata parsed;
        if (encoded.empty())
        {
            result.clear();
            return true;
        }

        const auto hexValue = [](char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        const auto decode = [&](const std::string& source, std::string& destination) -> bool
        {
            if (source.size() % 2)
                return false;

            destination.clear();
            for (size_t index = 0; index < source.size(); index += 2)
            {
                const auto high = hexValue(source[index]);
                const auto low = hexValue(source[index + 1]);
                if (high < 0 || low < 0)
                    return false;
                destination += char((high << 4) | low);
            }
            return true;
        };

        size_t begin = 0;
        while (begin < encoded.size())
        {
            const auto end = encoded.find(';', begin);
            const auto item = encoded.substr(begin, end == std::string::npos ? end : end - begin);
            const auto separator = item.find(':');
            if (separator == std::string::npos || item.find(':', separator + 1) != std::string::npos)
                return false;

            std::string key, value;
            if (!decode(item.substr(0, separator), key) || !decode(item.substr(separator + 1), value))
                return false;

            const String metadataKey(key.c_str());
            const String metadataValue(value.c_str());
            if (!validMetadataKey(metadataKey) || value.find('\0') != std::string::npos ||
                !parsed.insert({metadataKey, metadataValue}).second)
                return false;

            if (end == std::string::npos)
                break;
            begin = end + 1;
            if (begin == encoded.size())
                return false;
        }

        if (!validMetadata(parsed) || encodeSessionMetadata(parsed) != encoded)
            return false;

        result = parsed;
        return true;
    }

    // Version marker separates the existing roster format from generic session properties.
    static std::string appendSessionMetadata(const std::string& roster, const MultiplayerSessionMetadata& metadata)
    {
        return roster + "|M1|" + encodeSessionMetadata(metadata);
    }

    // Keep session metadata within the existing Carbon manifest budget.
    static bool validMetadataKey(const String& key)
    {
        if (key.length() == 0 || key.length() > 64)
            return false;

        for (unsigned int index = 0; index < key.length(); ++index)
        {
            const auto c = key.cStr()[index];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                return false;
        }
        return true;
    }

    static bool validMetadata(const MultiplayerSessionMetadata& metadata)
    {
        if (metadata.size() > 64)
            return false;

        size_t bytes = 0;
        for (const auto& entry : metadata)
        {
            if (!validMetadataKey(entry.first) || entry.second.length() > 256)
                return false;

            // Reserve serialization overhead; the provider checks the final manifest size.
            bytes += entry.first.length() + entry.second.length() + 12;
            if (bytes > 2048)
                return false;

            for (unsigned int index = 0; index < entry.second.length(); ++index)
            {
                if (entry.second.cStr()[index] == '\0')
                    return false;
            }
        }
        return true;
    }
};

}
}
/** \endcond */
