// A small JSON reader for what the tools print (tools/doctor.py --json, the `@vcs {...}` events of tools/toolchain.py).
// Reading only; numbers are doubles.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace launcher
{
    class Json
    {
    public:
        enum class Type
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        };

        static Json parse(std::string_view text, bool *ok = nullptr);

        Type type() const { return m_type; }
        bool isNull() const { return m_type == Type::Null; }
        bool isObject() const { return m_type == Type::Object; }
        bool has(const std::string &key) const { return m_object && m_object->count(key) != 0; }

        // A member, or a null value (so lookups chain: j["build"]["info"]["opt"].str())
        const Json &operator[](const std::string &key) const;
        const Json &operator[](size_t index) const;
        size_t size() const { return m_array ? m_array->size() : 0; }

        std::string str(const std::string &fallback = "") const { return m_type == Type::String ? m_string : fallback; }
        double num(double fallback = 0) const { return m_type == Type::Number ? m_number : fallback; }
        bool boolean(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
        const std::map<std::string, Json> &members() const;

    private:
        Type m_type = Type::Null;
        bool m_bool = false;
        double m_number = 0;
        std::string m_string;
        std::shared_ptr<std::vector<Json>> m_array;
        std::shared_ptr<std::map<std::string, Json>> m_object;

        friend class JsonParser;
    };
}
