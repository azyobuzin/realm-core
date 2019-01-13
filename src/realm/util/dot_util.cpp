#include <realm/util/dot_util.hpp>

using namespace std::literals::string_literals;

void replace_with(std::string& src, char old_value, const std::string& new_value)
{
    for (size_t offset = 0; offset < src.size();) {
        size_t found_pos = src.find(old_value, offset);

        if (found_pos == std::string::npos)
            return;

        src.replace(found_pos, 1, new_value);

        offset = found_pos + new_value.size();
    }
}

std::string dot_escape_html(const std::string& src)
{
    std::string s = src;
    replace_with(s, '&', "&amp;"s);
    replace_with(s, '<', "&lt;"s);
    replace_with(s, '>', "&gt;"s);
    replace_with(s, '"', "&quot;"s);
    replace_with(s, '\'', "&#x27;"s);
    return s;
}

std::string dot_escape_quote(const std::string& src)
{
    std::string s = src;
    replace_with(s, '"', "\\\""s);
    return s;
}
