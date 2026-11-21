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

    /** Identifies the stored parameter representation. */
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
    /** Constructs a boolean parameter. */
    NetFrameParameter(bool value) : type_(Boolean), integer_(value ? 1 : 0) {}
    /** Constructs a signed integer parameter. */
    NetFrameParameter(int value) : type_(Integer), integer_(value) {}
    /** Constructs an integer parameter from an unsigned value. */
    NetFrameParameter(unsigned int value) : type_(Integer), integer_(value) {}
    /** Constructs a signed 64-bit integer parameter. */
    NetFrameParameter(int64_t value) : type_(Integer), integer_(value) {}
    /** Constructs a single-precision floating-point parameter. */
    NetFrameParameter(float value) : type_(Float) { floats_[0] = value; }
    /** Constructs a string parameter by copying the supplied value. */
    NetFrameParameter(const String& value) : type_(StringValue), string_(value) {}
    /** Constructs a string parameter from a C string. */
    NetFrameParameter(const char* value) : type_(StringValue), string_(value) {}
    /** Constructs a two-component vector parameter. */
    NetFrameParameter(const Vec2& value) : type_(Vec2Value) { floats_[0] = value.x; floats_[1] = value.y; }
    /** Constructs a three-component vector parameter. */
    NetFrameParameter(const Vec3& value) : type_(Vec3Value)
    {
        floats_[0] = value.x;
        floats_[1] = value.y;
        floats_[2] = value.z;
    }
    /** Constructs a quaternion parameter. */
    NetFrameParameter(const Quaternion& value) : type_(QuaternionValue)
    {
        floats_[0] = value.x; floats_[1] = value.y; floats_[2] = value.z; floats_[3] = value.w;
    }
    /** Constructs a color parameter. */
    NetFrameParameter(const Color& value) : type_(ColorValue)
    {
        floats_[0] = value.r; floats_[1] = value.g; floats_[2] = value.b; floats_[3] = value.a;
    }
    /** Constructs a binary parameter by copying the supplied bytes. */
    NetFrameParameter(const Vector<byte_t>& value) : type_(BinaryValue), binary_(value) {}

    /** Constructs a parameter containing four floating-point components. */
    static NetFrameParameter fromFloat4(float x, float y, float z, float w)
    {
        NetFrameParameter value;
        value.type_ = Float4Value;
        value.floats_[0] = x; value.floats_[1] = y; value.floats_[2] = z; value.floats_[3] = w;
        return value;
    }

    /** Returns the stored parameter type. */
    Type getType() const { return type_; }
    /** Returns the boolean value; callers should check the parameter type first. */
    bool asBoolean() const { return integer_ != 0; }
    /** Returns the integer value narrowed to int; callers should check the parameter type first. */
    int asInteger() const { return int(integer_); }
    /** Returns the full signed 64-bit integer value; callers should check the parameter type first. */
    int64_t asInteger64() const { return integer_; }
    /** Returns the floating-point value; callers should check the parameter type first. */
    float asFloat() const { return floats_[0]; }
    /** Returns the stored string; callers should check the parameter type first. */
    const String& asString() const { return string_; }
    /** Returns the two-component vector; callers should check the parameter type first. */
    Vec2 asVec2() const { return {floats_[0], floats_[1]}; }
    /** Returns the three-component vector; callers should check the parameter type first. */
    Vec3 asVec3() const { return {floats_[0], floats_[1], floats_[2]}; }
    /** Returns the quaternion; callers should check the parameter type first. */
    Quaternion asQuaternion() const { return {floats_[0], floats_[1], floats_[2], floats_[3]}; }
    /** Returns the color; callers should check the parameter type first. */
    Color asColor() const { return {floats_[0], floats_[1], floats_[2], floats_[3]}; }
    /** Returns the pointer to the four stored floating-point components; callers should check the parameter type first. */
    const float* asFloat4() const { return floats_; }
    /** Returns the stored binary bytes; callers should check the parameter type first. */
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

    /** Application-defined message type identifier. */
    using Type = uint32_t;

    /** Constructs an empty frame with the supplied message type. */
    explicit NetFrame(Type type = 0) : type_(type) {}

    /** Returns the application-defined message type. */
    Type getType() const { return type_; }
    /** Sets the application-defined message type. */
    void setType(Type type) { type_ = type; }

    /** Set replaces an existing named parameter instead of appending a duplicate. */
    void set(const String& name, const NetFrameParameter& value);
    /** Returns the named parameter, or a null parameter if absent. */
    const NetFrameParameter& get(const String& name) const;
    /** Returns whether a parameter with the supplied name exists. */
    bool has(const String& name) const;
    /** Returns the number of named parameters. */
    unsigned int size() const { return parameters_.size(); }

    /** Encodes a bounded, versioned, little-endian binary message. */
    bool encode(Vector<byte_t>& bytes) const;
    /** Decodes a bounded versioned wire message; returns false for invalid input without replacing result. */
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
