#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace LuaHostTests
{
    inline void check(bool condition,
                      std::string_view expression,
                      const std::source_location location = std::source_location::current())
    {
        if (!condition)
        {
            throw std::runtime_error(std::string{location.file_name()} + ":" + std::to_string(location.line()) +
                                     ": assertion failed: " + std::string{expression});
        }
    }

    struct TestCase
    {
        std::string_view name;
        std::function<void()> run;
    };

    inline int run(const std::vector<TestCase>& cases, int argc, char** argv)
    {
        std::size_t repetitions = 1;
        if (argc == 3 && std::string_view{argv[1]} == "--repeat")
        {
            try
            {
                repetitions = std::stoul(argv[2]);
                if (repetitions == 0)
                {
                    throw std::invalid_argument("zero repetitions");
                }
            }
            catch (const std::exception&)
            {
                std::cerr << "--repeat requires a positive integer\n";
                return 2;
            }
        }
        else if (argc != 1)
        {
            std::cerr << "usage: " << argv[0] << " [--repeat N]\n";
            return 2;
        }

        std::size_t failures = 0;
        for (std::size_t repetition = 0; repetition < repetitions; ++repetition)
        {
            for (const auto& test : cases)
            {
                try
                {
                    test.run();
                    std::cout << "PASS " << test.name << " (run " << repetition + 1 << ")\n";
                }
                catch (const std::exception& error)
                {
                    ++failures;
                    std::cerr << "FAIL " << test.name << " (run " << repetition + 1 << "): " << error.what() << '\n';
                }
                catch (...)
                {
                    ++failures;
                    std::cerr << "FAIL " << test.name << " (run " << repetition + 1 << "): unknown exception\n";
                }
            }
        }
        std::cout << "RESULT " << cases.size() * repetitions << " executed, " << failures << " failed\n";
        return failures == 0 ? 0 : 1;
    }
} // namespace LuaHostTests

#define CHECK(expression) ::LuaHostTests::check(static_cast<bool>(expression), #expression)
