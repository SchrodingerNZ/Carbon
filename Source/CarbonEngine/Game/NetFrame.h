/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "CarbonEngine/Core/Vector.h"
#include "CarbonEngine/Core/StringBase.h"
#include "CarbonEngine/Math/Color.h"
#include "CarbonEngine/Math/Quaternion.h"
#include "CarbonEngine/Math/Vec2.h"
#include "CarbonEngine/Math/Vec3.h"

namespace Carbon
{

/** A typed, value-owned parameter suitable for transmission between machines. */
class CARBON_API NetFrameParameter
{
public:

    enum Type : uint8_t
    {
        Null,
        Boolean,
        Integer,
        Float,
        StringValue,
        Vec2Value,
        Vec3Value,
        QuaternionValue,
        ColorValue,
        Float4Value,
        BinaryValue
    };

    NetFrameParameter() = default;
    NetFrameParameter(bool value) : type_(Boolean), integer_(value ? 1 : 0) {}
    NetFrameParameter(int value) : type_(Integer), integer_(value) {}
    NetFrameParameter(unsigned int value) : type_(Integer), integer_(value) {}
    NetFrameParameter(int64_t value) : type_(Integer), integer_(value) {}
    NetFrameParameter(float value) : type_(Float) { floats_[0] = value; }
    NetFrameParameter(const String& value) : type_(StringValue), string_(value) {}
    NetFrameParameter(const char* value) : type_(StringValue), string_(value) {}
    NetFrameParameter(const Vec2& value) : type_(Vec2Value) { floats_[0] = value.x; floats_[1] = value.y; }
    NetFrameParameter(const Vec3& value) : type_(Vec3Value)
    {
        floats_[0] = value.x;
        floats_[1] = value.y;
        floats_[2] = value.z;
    }
    NetFrameParameter(const Quaternion& value) : type_(QuaternionValue)
    {
        floats_[0] = value.x; floats_[1] = value.y; floats_[2] = value.z; floats_[3] = value.w;
    }
    NetFrameParameter(const Color& value) : type_(ColorValue)
    {
        floats_[0] = value.r; floats_[1] = value.g; floats_[2] = value.b; floats_[3] = value.a;
    }
    NetFrameParameter(const Vector<byte_t>& value) : type_(BinaryValue), binary_(value) {}

    static NetFrameParameter fromFloat4(float x, float y, float z, float w)
    {
        NetFrameParameter value;
        value.type_ = Float4Value;
        value.floats_[0] = x; value.floats_[1] = y; value.floats_[2] = z; value.floats_[3] = w;
        return value;
    }

    Type getType() const { return type_; }
    bool asBoolean() const { return integer_ != 0; }
    int asInteger() const { return int(integer_); }
    int64_t asInteger64() const { return integer_; }
    float asFloat() const { return floats_[0]; }
    const String& asString() const { return string_; }
    Vec2 asVec2() const { return {floats_[0], floats_[1]}; }
    Vec3 asVec3() const { return {floats_[0], floats_[1], floats_[2]}; }
    Quaternion asQuaternion() const { return {floats_[0], floats_[1], floats_[2], floats_[3]}; }
    Color asColor() const { return {floats_[0], floats_[1], floats_[2], floats_[3]}; }
    const float* asFloat4() const { return floats_; }
    const Vector<byte_t>& asBinary() const { return binary_; }

private:

    Type type_ = Null;
    int64_t integer_ = 0;
    float floats_[4] = {};
    String string_;
    Vector<byte_t> binary_;
};

/** A typed application message, independent of the transport provider. */
class CARBON_API NetFrame
{
public:

    using Type = uint32_t;

    explicit NetFrame(Type type = 0) : type_(type) {}

    Type getType() const { return type_; }
    void setType(Type type) { type_ = type; }

    /** Set replaces an existing named parameter instead of appending a duplicate. */
    void set(const String& name, const NetFrameParameter& value);
    const NetFrameParameter& get(const String& name) const;
    bool has(const String& name) const;
    unsigned int size() const { return parameters_.size(); }

    /** Encode and decode a bounded, versioned, little-endian binary message. */
    bool encode(Vector<byte_t>& bytes) const;
    static bool decode(const byte_t* bytes, unsigned int length, NetFrame& result);

private:

    struct Entry
    {
        String name;
        NetFrameParameter value;
    };

    Type type_ = 0;
    Vector<Entry> parameters_;
};

}
