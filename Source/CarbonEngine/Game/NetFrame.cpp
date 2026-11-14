/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "CarbonEngine/Common.h"
#include "CarbonEngine/Game/NetFrame.h"
#include "CarbonEngine/Core/Parameter.h"

namespace Carbon
{

namespace
{

// Bound decoded allocations before accepting data supplied by another computer.
constexpr unsigned int MaxFrameSize = 1024 * 1024;
constexpr unsigned int MaxParameters = 256;
constexpr unsigned int MaxNameLength = 128;
constexpr unsigned int MaxValueLength = 256 * 1024;

void writeU8(Vector<byte_t>& output, uint8_t value) { output.append(value); }

void writeU32(Vector<byte_t>& output, uint32_t value)
{
    for (unsigned int i = 0; i < 4; i++)
        output.append(byte_t(value >> (i * 8)));
}

void writeU64(Vector<byte_t>& output, uint64_t value)
{
    for (unsigned int i = 0; i < 8; i++)
        output.append(byte_t(value >> (i * 8)));
}

void writeFloat(Vector<byte_t>& output, float value)
{
    static_assert(sizeof(float) == sizeof(uint32_t), "NetFrame requires 32-bit floats");

    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    writeU32(output, bits);
}

bool readU8(const byte_t*& current, const byte_t* end, uint8_t& value)
{
    if (current == end)
        return false;

    value = *current++;
    return true;
}

bool readU32(const byte_t*& current, const byte_t* end, uint32_t& value)
{
    if (end - current < 4)
        return false;

    value = 0;
    for (unsigned int i = 0; i < 4; i++)
        value |= uint32_t(*current++) << (i * 8);

    return true;
}

bool readU64(const byte_t*& current, const byte_t* end, uint64_t& value)
{
    if (end - current < 8)
        return false;

    value = 0;
    for (unsigned int i = 0; i < 8; i++)
        value |= uint64_t(*current++) << (i * 8);

    return true;
}

bool readFloat(const byte_t*& current, const byte_t* end, float& value)
{
    uint32_t bits = 0;
    if (!readU32(current, end, bits))
        return false;

    memcpy(&value, &bits, sizeof(value));
    return true;
}

bool writeString(Vector<byte_t>& output, const String& string, unsigned int maximum)
{
    // Carbon::String stores narrow characters. The wire format preserves their byte representation.
    if (string.length() > maximum)
        return false;

    writeU32(output, string.length());
    for (unsigned int i = 0; i < string.length(); i++)
        output.append(byte_t(string.at(i)));
    return true;
}

bool readString(const byte_t*& current, const byte_t* end, unsigned int maximum, String& result)
{
    uint32_t count = 0;
    if (!readU32(current, end, count) || count > maximum || static_cast<size_t>(end - current) < count)
        return false;

    // Construct from a bounded, null-terminated copy rather than reading beyond the frame.
    Vector<char> text(count + 1, 0);
    for (unsigned int i = 0; i < count; i++)
        text[i] = char(current[i]);

    // Embedded nulls are not representable in Carbon::String.
    if (memchr(text.getData(), 0, count))
        return false;

    result = String(text.getData());
    current += count;
    return true;
}

unsigned int floatCount(NetFrameParameter::Type type)
{
    switch (type)
    {
        case NetFrameParameter::Float:           return 1;
        case NetFrameParameter::Vec2Value:       return 2;
        case NetFrameParameter::Vec3Value:       return 3;
        case NetFrameParameter::QuaternionValue:
        case NetFrameParameter::ColorValue:
        case NetFrameParameter::Float4Value:     return 4;
        default:                                 return 0;
    }
}

}

void NetFrame::set(const String& name, const NetFrameParameter& value)
{
    for (auto& entry : parameters_)
    {
        if (entry.name == name)
        {
            entry.value = value;
            return;
        }
    }

    parameters_.append({name, value});
}

const NetFrameParameter& NetFrame::get(const String& name) const
{
    for (const auto& entry : parameters_)
    {
        if (entry.name == name)
            return entry.value;
    }

    static const NetFrameParameter empty;
    return empty;
}

bool NetFrame::has(const String& name) const
{
    for (const auto& entry : parameters_)
    {
        if (entry.name == name)
            return true;
    }

    return false;
}

bool NetFrame::encode(Vector<byte_t>& result) const
{
    // Build into a temporary buffer so invalid parameters never produce a partial frame.
    Vector<byte_t> bytes;

    bytes.append('N'); bytes.append('F'); bytes.append('R'); bytes.append('M');
    writeU8(bytes, 1);
    writeU32(bytes, type_);

    if (parameters_.size() > MaxParameters)
        return false;

    writeU32(bytes, parameters_.size());

    for (const auto& entry : parameters_)
    {
        if (!writeString(bytes, entry.name, MaxNameLength) || !entry.name.length() || !Parameter::isValidParameterName(entry.name))
            return false;

        const auto& value = entry.value;
        auto type = value.getType();
        writeU8(bytes, type);

        switch (type)
        {
            case NetFrameParameter::Null:
                break;

            case NetFrameParameter::Boolean:
                writeU8(bytes, value.asBoolean() ? 1 : 0);
                break;

            case NetFrameParameter::Integer:
                writeU64(bytes, uint64_t(value.asInteger64()));
                break;

            case NetFrameParameter::StringValue:
                if (!writeString(bytes, value.asString(), MaxValueLength))
                    return false;
                break;

            case NetFrameParameter::BinaryValue:
            {
                const auto& binary = value.asBinary();
                if (binary.size() > MaxValueLength)
                    return false;

                writeU32(bytes, binary.size());
                bytes.append(binary);
                break;
            }

            default:
            {
                const unsigned int count = floatCount(type);
                if (!count)
                    return false;

                for (unsigned int i = 0; i < count; i++)
                    writeFloat(bytes, value.asFloat4()[i]);

                break;
            }
        }

        if (bytes.size() > MaxFrameSize)
            return false;
    }

    result = std::move(bytes);
    return true;
}

bool NetFrame::decode(const byte_t* bytes, unsigned int length, NetFrame& result)
{
    if (!bytes || length < 13 || length > MaxFrameSize)
        return false;

    const byte_t* current = bytes;
    const byte_t* end = bytes + length;

    // Confirm the frame signature and wire-format version.
    if (current[0] != 'N' || current[1] != 'F' || current[2] != 'R' || current[3] != 'M' || current[4] != 1)
        return false;

    current += 5;

    uint32_t type = 0;
    uint32_t count = 0;
    if (!readU32(current, end, type) || !readU32(current, end, count) || count > MaxParameters)
        return false;

    NetFrame frame(type);

    for (unsigned int i = 0; i < count; i++)
    {
        String name;
        if (!readString(current, end, MaxNameLength, name) || !name.length() || !Parameter::isValidParameterName(name) || frame.has(name))
            return false;

        uint8_t rawType = 0;
        if (!readU8(current, end, rawType) || rawType > NetFrameParameter::BinaryValue)
            return false;

        const auto parameterType = NetFrameParameter::Type(rawType);
        NetFrameParameter parameter;

        switch (parameterType)
        {
            case NetFrameParameter::Null:
                break;

            case NetFrameParameter::Boolean:
            {
                uint8_t value = 0;
                if (!readU8(current, end, value) || value > 1)
                    return false;

                parameter = NetFrameParameter(value == 1);
                break;
            }

            case NetFrameParameter::Integer:
            {
                uint64_t value = 0;
                if (!readU64(current, end, value))
                    return false;

                parameter = NetFrameParameter(int64_t(value));
                break;
            }

            case NetFrameParameter::StringValue:
            {
                String value;
                if (!readString(current, end, MaxValueLength, value))
                    return false;

                parameter = NetFrameParameter(value);
                break;
            }

            case NetFrameParameter::BinaryValue:
            {
                uint32_t size = 0;
                if (!readU32(current, end, size) || size > MaxValueLength || static_cast<size_t>(end - current) < size)
                    return false;

                Vector<byte_t> data;
                for (unsigned int j = 0; j < size; j++)
                    data.append(*current++);

                parameter = NetFrameParameter(data);
                break;
            }

            default:
            {
                const unsigned int components = floatCount(parameterType);
                if (!components)
                    return false;

                float values[4] = {};
                for (unsigned int j = 0; j < components; j++)
                {
                    if (!readFloat(current, end, values[j]))
                        return false;
                }

                switch (parameterType)
                {
                    case NetFrameParameter::Float:           parameter = NetFrameParameter(values[0]); break;
                    case NetFrameParameter::Vec2Value:       parameter = NetFrameParameter(Vec2(values[0], values[1])); break;
                    case NetFrameParameter::Vec3Value:       parameter = NetFrameParameter(Vec3(values[0], values[1], values[2])); break;
                    case NetFrameParameter::QuaternionValue: parameter = NetFrameParameter(Quaternion(values[0], values[1], values[2], values[3])); break;
                    case NetFrameParameter::ColorValue:      parameter = NetFrameParameter(Color(values[0], values[1], values[2], values[3])); break;
                    case NetFrameParameter::Float4Value:     parameter = NetFrameParameter::fromFloat4(values[0], values[1], values[2], values[3]); break;
                    default:                                 return false;
                }

                break;
            }
        }

        frame.set(name, parameter);
    }

    // Reject trailing data, as well as malformed and truncated messages.
    if (current != end)
        return false;

    result = std::move(frame);
    return true;
}

}
