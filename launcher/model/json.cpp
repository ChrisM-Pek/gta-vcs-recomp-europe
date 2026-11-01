#include "json.h"

#include <cstdlib>

namespace launcher
{
    namespace
    {
        const Json kNull;
        const std::map<std::string, Json> kNoMembers;

        void appendUtf8(std::string &out, unsigned cp)
        {
            if (cp < 0x80)
                out += static_cast<char>(cp);
            else if (cp < 0x800)
            {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
    }

    class JsonParser
    {
    public:
        explicit JsonParser(std::string_view text) : m_text(text) {}

        bool parse(Json &out)
        {
            if (!value(out, 0))
                return false;
            skip();
            return m_pos == m_text.size();
        }

    private:
        std::string_view m_text;
        size_t m_pos = 0;

        void skip()
        {
            while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' ||
                                              m_text[m_pos] == '\r'))
                ++m_pos;
        }

        bool literal(std::string_view word)
        {
            if (m_text.substr(m_pos, word.size()) != word)
                return false;
            m_pos += word.size();
            return true;
        }

        bool hex4(unsigned &cp)
        {
            if (m_pos + 4 > m_text.size())
                return false;
            cp = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char c = m_text[m_pos++];
                cp <<= 4;
                if (c >= '0' && c <= '9')
                    cp |= unsigned(c - '0');
                else if (c >= 'a' && c <= 'f')
                    cp |= unsigned(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    cp |= unsigned(c - 'A' + 10);
                else
                    return false;
            }
            return true;
        }

        bool string(std::string &out)
        {
            if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                return false;
            ++m_pos;
            while (m_pos < m_text.size())
            {
                const char c = m_text[m_pos++];
                if (c == '"')
                    return true;
                if (c != '\\')
                {
                    out += c;
                    continue;
                }
                if (m_pos >= m_text.size())
                    return false;
                const char e = m_text[m_pos++];
                switch (e)
                {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                {
                    unsigned cp = 0;
                    if (!hex4(cp))
                        return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && literal("\\u")) // a surrogate pair
                    {
                        unsigned low = 0;
                        if (!hex4(low))
                            return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
                }
            }
            return false;
        }

        bool value(Json &out, int depth)
        {
            if (depth > 64)
                return false;
            skip();
            if (m_pos >= m_text.size())
                return false;
            const char c = m_text[m_pos];
            if (c == '{')
            {
                ++m_pos;
                out.m_type = Json::Type::Object;
                out.m_object = std::make_shared<std::map<std::string, Json>>();
                skip();
                if (m_pos < m_text.size() && m_text[m_pos] == '}')
                    return ++m_pos, true;
                for (;;)
                {
                    skip();
                    std::string key;
                    if (!string(key))
                        return false;
                    skip();
                    if (m_pos >= m_text.size() || m_text[m_pos++] != ':')
                        return false;
                    if (!value((*out.m_object)[key], depth + 1))
                        return false;
                    skip();
                    if (m_pos >= m_text.size())
                        return false;
                    if (m_text[m_pos] == ',')
                    {
                        ++m_pos;
                        continue;
                    }
                    return m_text[m_pos++] == '}';
                }
            }
            if (c == '[')
            {
                ++m_pos;
                out.m_type = Json::Type::Array;
                out.m_array = std::make_shared<std::vector<Json>>();
                skip();
                if (m_pos < m_text.size() && m_text[m_pos] == ']')
                    return ++m_pos, true;
                for (;;)
                {
                    out.m_array->emplace_back();
                    if (!value(out.m_array->back(), depth + 1))
                        return false;
                    skip();
                    if (m_pos >= m_text.size())
                        return false;
                    if (m_text[m_pos] == ',')
                    {
                        ++m_pos;
                        continue;
                    }
                    return m_text[m_pos++] == ']';
                }
            }
            if (c == '"')
            {
                out.m_type = Json::Type::String;
                return string(out.m_string);
            }
            if (literal("true"))
                return out.m_type = Json::Type::Bool, out.m_bool = true, true;
            if (literal("false"))
                return out.m_type = Json::Type::Bool, out.m_bool = false, true;
            if (literal("null"))
                return out.m_type = Json::Type::Null, true;
            const std::string rest(m_text.substr(m_pos, 64));
            char *end = nullptr;
            const double number = std::strtod(rest.c_str(), &end);
            if (end == rest.c_str())
                return false;
            m_pos += static_cast<size_t>(end - rest.c_str());
            out.m_type = Json::Type::Number;
            out.m_number = number;
            return true;
        }
    };

    Json Json::parse(std::string_view text, bool *ok)
    {
        Json out;
        const bool good = JsonParser(text).parse(out);
        if (ok)
            *ok = good;
        return good ? out : Json();
    }

    const Json &Json::operator[](const std::string &key) const
    {
        if (!m_object)
            return kNull;
        const auto it = m_object->find(key);
        return it == m_object->end() ? kNull : it->second;
    }

    const Json &Json::operator[](size_t index) const
    {
        return m_array && index < m_array->size() ? (*m_array)[index] : kNull;
    }

    const std::map<std::string, Json> &Json::members() const
    {
        return m_object ? *m_object : kNoMembers;
    }
}
