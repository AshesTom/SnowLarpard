// SPDX-License-Identifier: LGPL-2.1-or-later
#include "engine.h"

#include "foundation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

namespace Calculator {

namespace {

constexpr long double PiL = 3.141592653589793238462643383279502884L;
constexpr long double EL = 2.718281828459045235360287471352662498L;
constexpr size_t MaxDigits = 16;   // the digits a number typed may have

std::string between(const std::string &s, size_t &at, const std::string &open, const std::string &close)
{
    const size_t a = s.find(open, at);
    if (a == std::string::npos)
        return {};
    const size_t b = s.find(close, a + open.size());
    if (b == std::string::npos)
        return {};
    at = b + close.size();
    return s.substr(a + open.size(), b - a - open.size());
}

std::string unescape(std::string s)
{
    const std::pair<const char *, const char *> entities[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto &[from, to] : entities)
        for (size_t i; (i = s.find(from)) != std::string::npos;)
            s.replace(i, std::string(from).size(), to);
    return s;
}

long double parseTyped(const std::string &mantissa, const std::string &exponent)
{
    std::string s = mantissa.empty() || mantissa == "-" ? "0" : mantissa;
    if (!exponent.empty() && exponent != "-")
        s += "e" + exponent;
    return std::strtold(s.c_str(), nullptr);
}

std::string withSeparators(const std::string &number)
{
    // the integer part's thousands, the sign and the fraction kept
    size_t start = number[0] == '-' ? 1 : 0;
    size_t end = number.find_first_of(".e", start);
    if (end == std::string::npos)
        end = number.size();
    std::string out = number.substr(0, start);
    const std::string digits = number.substr(start, end - start);
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return out + number.substr(end);
}

} // namespace

std::string formatNumber(long double v, int decimals, bool separators)
{
    if (v == 0 || std::fabs(v) < 1e-300L)
        return "0";
    char buf[128];
    const long double a = std::fabs(v);
    std::string s;
    if (a >= 1e16L || a < std::pow(10.0L, -decimals)) {
        // an exponent: the mantissa with its zeros taken off
        std::snprintf(buf, sizeof buf, "%.*Le", std::min(decimals, 15), v);
        s = buf;
        const size_t e = s.find('e');
        std::string mantissa = s.substr(0, e), exp = s.substr(e + 1);
        if (mantissa.find('.') != std::string::npos) {
            while (mantissa.back() == '0')
                mantissa.pop_back();
            if (mantissa.back() == '.')
                mantissa.pop_back();
        }
        const int power = std::atoi(exp.c_str());
        return mantissa + "e" + std::to_string(power);
    }
    std::snprintf(buf, sizeof buf, "%.*Lf", decimals, v);
    s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (s == "-0")
        s = "0";
    return separators ? withSeparators(s) : s;
}

bool Engine::load(const std::string &functionsXml)
{
    std::ifstream f(functionsXml);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string xml = ss.str();
    // <array> of <dict>: <key>name</key><string>+</string>, <key>operator</key><integer>8</integer>...
    size_t at = 0;
    for (std::string dict; !(dict = between(xml, at, "<dict>", "</dict>")).empty();) {
        Function fn;
        size_t k = 0;
        for (std::string key; !(key = between(dict, k, "<key>", "</key>")).empty();) {
            const size_t valueAt = dict.find('<', k);
            if (valueAt == std::string::npos)
                break;
            const size_t tagEnd = dict.find('>', valueAt);
            const std::string tag = dict.substr(valueAt + 1, tagEnd - valueAt - 1);
            const std::string value = unescape(between(dict, k, "<" + tag + ">", "</" + tag + ">"));
            if (key == "name")
                fn.name = value;
            else if (key == "selector")
                fn.selector = value;
            else if (key == "operator")
                fn.op = std::atoi(value.c_str());
            else if (key == "paramsRequired")
                fn.params = std::atoi(value.c_str());
            else if (key == "peekParameters")
                fn.peek = std::atoi(value.c_str());
            else if (key == "precedence")
                fn.precedence = std::atoi(value.c_str());
        }
        m_functions.push_back(fn);
    }
    return !m_functions.empty();
}

const Function *Engine::function(int code) const
{
    for (const Function &f : m_functions)
        if (f.op == code)
            return &f;
    return nullptr;
}

void Engine::clear()
{
    // -[CalculatorEngine clear]: all but the memory
    m_postfix.clear();
    m_ops.clear();
    m_lastToken = NoToken;
    m_lastTokenOp = 0;
    m_hasLastNumber = false;
    m_lastFunction = nullptr;
    m_leftParens = 0;
    m_input.clear();
    m_exponentInput.clear();
    m_inExponent = false;
    m_shown = 0;
    m_error.clear();
}

void Engine::digit(int d)
{
    if (error())
        clear();
    if (m_inExponent) {
        if (m_exponentInput.size() < 3 + (m_exponentInput[0] == '-' ? 1 : 0))
            m_exponentInput += char('0' + d);
        m_shown = parseTyped(m_input, m_exponentInput);
        return;
    }
    const size_t digits = size_t(std::count_if(m_input.begin(), m_input.end(), [](char c) { return c >= '0' && c <= '9'; }));
    if (m_input.empty() || m_input == "0")
        m_input = std::string(1, char('0' + d));
    else if (m_input == "-0")
        m_input = "-" + std::string(1, char('0' + d));
    else if (digits < MaxDigits)
        m_input += char('0' + d);
    m_shown = parseTyped(m_input, {});
}

void Engine::decimalPoint()
{
    if (error())
        clear();
    if (m_inExponent)
        return;
    if (m_input.empty())
        m_input = "0.";
    else if (m_input.find('.') == std::string::npos)
        m_input += '.';
}

void Engine::exponent()
{
    // -[CalculatorEngine userEnteredEEMode]: the number shown is the mantissa
    if (error())
        return beep ? beep() : void();
    if (m_input.empty())
        m_input = formatNumber(m_shown, 15, false);
    m_inExponent = true;
    m_exponentInput.clear();
}

void Engine::deleteDigit()
{
    if (m_inExponent) {
        if (!m_exponentInput.empty())
            m_exponentInput.pop_back();
        else
            m_inExponent = false;
        m_shown = parseTyped(m_input, m_exponentInput);
        return;
    }
    if (m_input.empty())
        return;
    m_input.pop_back();
    if (m_input.empty() || m_input == "-")
        m_input = "0";
    m_shown = parseTyped(m_input, {});
}

void Engine::negate()
{
    if (!m_input.empty()) {
        std::string &s = m_inExponent ? m_exponentInput : m_input;
        if (!s.empty() && s[0] == '-')
            s.erase(0, 1);
        else
            s.insert(0, "-");
        m_shown = parseTyped(m_input, m_exponentInput);
        return;
    }
    op(Negate);
}

void Engine::number(long double v)
{
    m_input.clear();
    m_inExponent = false;
    enterNumber(v);
}

void Engine::enterTyped()
{
    if (m_input.empty())
        return;
    const long double v = parseTyped(m_input, m_inExponent ? m_exponentInput : std::string());
    m_input.clear();
    m_exponentInput.clear();
    m_inExponent = false;
    enterNumber(v);
}

void Engine::enterNumber(long double v)
{
    // -[AlgebraicEngine userEnteredToken:] for a number: it takes the place of the list's last one
    // after a result, or after another number
    m_error.clear();
    if ((m_lastToken == NoToken || m_ops.empty() || m_lastToken == NumberToken) && !m_postfix.empty())
        m_postfix.pop_back();
    m_postfix.push_back({true, v, nullptr});
    m_lastToken = NumberToken;
    m_shown = v;
}

void Engine::op(int code)
{
    const Function *f = function(code);
    if (!f)
        return;
    // -[CalculatorEngine userEnteredToken:]: clear, the memory, and the constants
    if (code == Clear) {
        clear();
        return;
    }
    if (code == MemPlus || code == MemMinus || code == MemClear || code == MemRecall) {
        // -[CalculatorEngine performMemoryOperation:]
        if (error() && code != MemClear && code != MemRecall)
            return beep ? beep() : void();
        if (code == MemClear) {
            m_memory = 0;
        } else if (code == MemRecall) {
            number(m_memory);
        } else {
            enterTyped();
            m_memory += code == MemPlus ? m_shown : -m_shown;
        }
        return;
    }
    if (f->params == 0 && !f->selector.empty()) {
        number(perform(f, {}));
        return;
    }
    // BasicAdvancedController handleOperatorPress: the number being typed first
    enterTyped();
    if (error())
        return beep ? beep() : void();
    if (code == Equals) {
        if (m_leftParens > 0) {
            while (m_leftParens > 0)
                op(RightParen);
            if (m_ops.empty())
                return;
        }
        if (!m_ops.empty()) {
            while (!m_ops.empty()) {
                m_postfix.push_back({false, 0, m_ops.back()});
                m_ops.pop_back();
            }
        } else if (m_hasLastNumber && m_lastFunction && m_postfix.size() == 1) {
            // = again: the last operation, with its last operand
            m_postfix.push_back({true, m_lastNumber, nullptr});
            m_postfix.push_back({false, 0, m_lastFunction});
        }
        evaluate();
    } else if (f->precedence == 99) {
        // after an operator, the function applies to the number before it (2 + √: √2)
        if (m_lastToken == OperatorToken && !m_postfix.empty() && m_lastTokenOp != Equals && m_lastTokenOp != RightParen)
            m_postfix.push_back(m_postfix.back());
        immediate(f);
        return;
    } else if (code == LeftParen) {
        // not straight after a number (2 ( is refused)
        if (m_lastToken == NumberToken)
            return beep ? beep() : void();
        ++m_leftParens;
        m_ops.push_back(f);
        return;
    } else if (code == RightParen) {
        if (m_leftParens == 0)
            return beep ? beep() : void();
        bool popped = false;
        while (!m_ops.empty() && m_ops.back()->op != LeftParen) {
            m_postfix.push_back({false, 0, m_ops.back()});
            m_ops.pop_back();
            popped = true;
        }
        if (!m_ops.empty())
            m_ops.pop_back();   // its "("
        --m_leftParens;
        if (popped) {
            evaluate();
            m_lastFunction = !m_ops.empty() && m_ops.back()->op != LeftParen ? m_ops.back() : nullptr;
        }
        return;
    } else {
        // a binary operator: another one just pressed is replaced
        if (m_lastToken == OperatorToken && !m_ops.empty() && m_ops.back()->op != LeftParen)
            m_ops.pop_back();
        bool popped = false;
        while (!m_ops.empty() && m_ops.back()->precedence >= f->precedence) {
            m_postfix.push_back({false, 0, m_ops.back()});
            m_ops.pop_back();
            popped = true;
        }
        if (popped)
            evaluate();
        m_ops.push_back(f);
        m_lastFunction = f;
    }
    m_lastToken = OperatorToken;
    m_lastTokenOp = code;
}

void Engine::evaluate()
{
    // -[AlgebraicEngine evaluateTOS]: the list, left to right, on a stack; a function short of
    // operands takes the last number seen (2 + = is 2 + 2)
    std::vector<long double> stack;
    bool seen = false;
    long double last = 0;
    for (const Item &it : m_postfix) {
        if (it.isNumber) {
            stack.push_back(it.value);
            last = it.value;
            seen = true;
            continue;
        }
        std::vector<long double> args;
        for (int i = 0; i < it.f->params; ++i) {
            if (stack.empty()) {
                args.push_back(seen ? last : 0);
            } else {
                args.insert(args.begin(), stack.back());
                stack.pop_back();
            }
        }
        stack.push_back(perform(it.f, args));
        if (error())
            break;
    }
    if (seen) {
        m_lastNumber = last;
        m_hasLastNumber = true;
    }
    m_postfix.clear();
    if (error())
        return;
    const long double r = stack.empty() ? 0 : stack.back();
    if (!stack.empty())
        stack.pop_back();
    for (long double v : stack)
        m_postfix.push_back({true, v, nullptr});
    result(r);
}

void Engine::immediate(const Function *f)
{
    // -[AlgebraicEngine immediateFunction:]: its operands from the list's end, then the numbers it
    // peeks at before them (% of the number before)
    std::vector<long double> args;
    for (int i = 0; i < f->params; ++i) {
        if (m_postfix.empty() || !m_postfix.back().isNumber)
            continue;
        args.insert(args.begin(), m_postfix.back().value);
        m_postfix.pop_back();
    }
    for (int i = 1; i <= f->peek && size_t(i) <= m_postfix.size(); ++i) {
        const Item &it = m_postfix[m_postfix.size() - size_t(i)];
        if (it.isNumber)
            args.insert(args.begin(), it.value);
    }
    if (args.empty())
        args.push_back(m_shown);
    const long double v = perform(f, args);
    if (!error())
        result(v);
}

void Engine::result(long double v)
{
    // -[AlgebraicEngine processOperationResult:]
    if (std::isnan(v))
        return fail("NaN");
    if (std::isinf(v))
        return fail("Over");
    m_postfix.push_back({true, v, nullptr});
    m_shown = v;
}

void Engine::fail(const std::string &key)
{
    if (m_error.empty())
        m_error = key;
    m_postfix.clear();
    m_ops.clear();
    m_leftParens = 0;
}

long double Engine::perform(const Function *f, const std::vector<long double> &args)
{
    // Function's selectors (BasicAndSci's -[Function plus:]...), on long double
    const std::string &s = f->selector;
    auto arg = [&args](size_t i) { return i < args.size() ? args[i] : 0.0L; };
    const long double x = args.empty() ? 0 : args.back();
    const long double toRad = radians ? 1 : PiL / 180, fromRad = radians ? 1 : 180 / PiL;
    if (s == "plus:")
        return arg(0) + arg(1);
    if (s == "minus:")
        return arg(0) - arg(1);
    if (s == "times:")
        return arg(0) * arg(1);
    if (s == "divide:") {
        if (arg(1) == 0) {
            fail("DivByZero");
            return 0;
        }
        return arg(0) / arg(1);
    }
    if (s == "power:")
        return std::pow(arg(0), arg(1));
    if (s == "xrooty:") {
        if (arg(1) == 0) {
            fail("DivByZero");
            return 0;
        }
        // an odd root of a negative number is real
        const long double r = 1 / arg(1);
        if (arg(0) < 0 && std::fmod(std::fabs(arg(1)), 2.0L) == 1)
            return -std::pow(-arg(0), r);
        return std::pow(arg(0), r);
    }
    if (s == "changeSign:")
        return -x;
    if (s == "percent:")
        return args.size() > 1 ? arg(0) * arg(1) / 100 : x / 100;
    if (s == "oneOverX:") {
        if (x == 0) {
            fail("DivByZero");
            return 0;
        }
        return 1 / x;
    }
    if (s == "squared:")
        return x * x;
    if (s == "cubed:")
        return x * x * x;
    if (s == "sqrt:")
        return std::sqrt(x);
    if (s == "cuberoot:")
        return std::cbrt(x);
    if (s == "factorial:") {
        if (x < 0 || x > 1754)
            return x < 0 ? NAN : INFINITY;
        return x == std::floor(x) ? std::round(std::tgamma(x + 1)) : std::tgamma(x + 1);
    }
    if (s == "sin:")
        return std::sin(x * toRad);
    if (s == "cos:")
        return std::cos(x * toRad);
    if (s == "tan:")
        return std::tan(x * toRad);
    if (s == "asin:")
        return std::asin(x) * fromRad;
    if (s == "acos:")
        return std::acos(x) * fromRad;
    if (s == "atan:")
        return std::atan(x) * fromRad;
    if (s == "sinh:")
        return std::sinh(x);
    if (s == "cosh:")
        return std::cosh(x);
    if (s == "tanh:")
        return std::tanh(x);
    if (s == "asinh:")
        return std::asinh(x);
    if (s == "acosh:")
        return std::acosh(x);
    if (s == "atanh:")
        return std::atanh(x);
    if (s == "log:")
        return std::log10(x);
    if (s == "ln:")
        return std::log(x);
    if (s == "log2:")
        return std::log2(x);
    if (s == "tentox:")
        return std::pow(10.0L, x);
    if (s == "etox:")
        return std::exp(x);
    if (s == "twoToPow:")
        return std::pow(2.0L, x);
    if (s == "pi")
        return PiL;
    if (s == "e")
        return EL;
    if (s == "random") {
        static std::mt19937_64 gen{std::random_device{}()};
        return std::uniform_real_distribution<long double>(0, 1)(gen);
    }
    return x;
}

long double Engine::value() const { return m_shown; }

std::string Engine::display(int decimals, bool separators) const
{
    if (error())
        return AK::Assets::tr("/Applications/Calculator.app", "Localizable", m_error, m_error);
    if (!m_input.empty()) {
        std::string s = separators ? withSeparators(m_input) : m_input;
        if (m_inExponent)
            s += "e" + m_exponentInput;
        return s;
    }
    return formatNumber(m_shown, decimals, separators);
}

} // namespace Calculator
