#include <base/LineReader.h>

#include <iostream>
#include <string_view>

#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>


#ifdef HAS_RESERVED_IDENTIFIER
#pragma clang diagnostic ignored "-Wreserved-identifier"
#endif

namespace
{

/// Trim ending whitespace inplace
void trim(String & s)
{
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) { return !std::isspace(ch); }).base(), s.end());
}

/// Check if multi-line query is inserted from the paste buffer.
/// Allows delaying the start of query execution until the entirety of query is inserted.
bool hasInputData()
{
    timeval timeout = {0, 0};
    fd_set fds{};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(1, &fds, nullptr, nullptr, &timeout) == 1;
}

}

replxx::Replxx::completions_t LineReader::Suggest::getCompletions(const String & prefix, size_t prefix_length)
{
    std::string_view last_word;

    auto last_word_pos = prefix.find_last_of(word_break_characters);
    if (std::string::npos == last_word_pos)
        last_word = prefix;
    else
        last_word = std::string_view(prefix).substr(last_word_pos + 1, std::string::npos);
    /// last_word can be empty.

    std::pair<Words::const_iterator, Words::const_iterator> range;

    std::lock_guard lock(mutex);

    /// Only perform case sensitive completion when the prefix string contains any uppercase characters
    if (std::none_of(prefix.begin(), prefix.end(), [&](auto c) { return c >= 'A' && c <= 'Z'; }))
        range = std::equal_range(
            words_no_case.begin(), words_no_case.end(), last_word, [prefix_length](std::string_view s, std::string_view prefix_searched)
            {
                return strncasecmp(s.data(), prefix_searched.data(), prefix_length) < 0;
            });
    else
        range = std::equal_range(words.begin(), words.end(), last_word, [prefix_length](std::string_view s, std::string_view prefix_searched)
        {
            return strncmp(s.data(), prefix_searched.data(), prefix_length) < 0;
        });

    return replxx::Replxx::completions_t(range.first, range.second);
}

void LineReader::Suggest::addWords(Words && new_words)
{
    std::lock_guard lock(mutex);

    /// Optimization for initial load.
    if (words.empty())
    {
        words.swap(new_words);
        words_no_case = words;
    }
    else
    {
        for (const auto & new_word : new_words)
        {
            if (std::binary_search(words.begin(), words.end(), new_word))
                continue;

            words.push_back(new_word);
            words_no_case.push_back(new_word);
        }
    }

    std::sort(words.begin(), words.end());
    std::sort(words_no_case.begin(), words_no_case.end(), [](const std::string & str1, const std::string & str2)
    {
        return std::lexicographical_compare(begin(str1), end(str1), begin(str2), end(str2), [](const char char1, const char char2)
        {
            return std::tolower(char1) < std::tolower(char2);
        });
    });
}

LineReader::LineReader(const String & history_file_path_, bool multiline_, Patterns extenders_, Patterns delimiters_)
    : history_file_path(history_file_path_), multiline(multiline_), extenders(std::move(extenders_)), delimiters(std::move(delimiters_))
{
    /// FIXME: check extender != delimiter
}

String LineReader::readLine(const String & first_prompt, const String & second_prompt)
{
    String line;
    bool need_next_line = false;

    while (auto status = readOneLine(need_next_line ? second_prompt : first_prompt))
    {
        if (status == RESET_LINE)
        {
            line.clear();
            need_next_line = false;
            continue;
        }

        if (input.empty())
        {
            if (!line.empty() && !multiline && !hasInputData())
                break;
            else
                continue;
        }

        const char * has_extender = nullptr;
        for (const auto * extender : extenders)
        {
            if (input.ends_with(extender))
            {
                has_extender = extender;
                break;
            }
        }

        const char * has_delimiter = nullptr;
        for (const auto * delimiter : delimiters)
        {
            if (input.ends_with(delimiter))
            {
                has_delimiter = delimiter;
                break;
            }
        }

        need_next_line = has_extender || (multiline && !has_delimiter) || hasInputData();

        if (has_extender)
        {
            input.resize(input.size() - strlen(has_extender));
            trim(input);
            if (input.empty())
                continue;
        }

        line += (line.empty() ? "" : "\n") + input;

        if (!need_next_line)
            break;
    }

    if (!line.empty() && line != prev_line)
    {
        addToHistory(line);
        prev_line = line;
    }

    return line;
}

LineReader::InputStatus LineReader::readOneLine(const String & prompt)
{
    input.clear();

    {
        std::cout << prompt;
        std::getline(std::cin, input);
        if (!std::cin.good())
            return ABORT;
    }

    trim(input);
    return INPUT_LINE;
}
