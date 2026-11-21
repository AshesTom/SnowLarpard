// SPDX-License-Identifier: LGPL-2.1-or-later
// Calculator's algebraic engine, as BasicAndSci.calcview's AlgebraicEngine (Calculator 4.5, 10.6) works:
// the keys are tokens, numbers and functions (Functions.xml: each function's operator code, number of
// parameters, precedence and selector). Numbers and functions go into a postfix list; the binary
// operators wait on an operator stack, popped onto the list while the stack's top has a precedence at
// least the new one's, and the list is then evaluated (evaluateTOS), its result shown and left as the
// list's last number. A function of precedence 99 applies at once to the number shown
// (immediateFunction:); % also takes the number before it. "=" pops every operator (closing the open
// parentheses) and evaluates; pressed again, it repeats the last operation with its last operand.
//
// Apple's engine works on NSDecimalNumber (38 digits); this one on long double.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Calculator {

// Functions.xml's entries
struct Function {
    std::string name;       // "+", "sin", "y^x"...
    std::string selector;   // "plus:", "sin:"...; empty for the engine's own (clear, =, parentheses, memory)
    std::string speechName; // what Speech says for its key
    int op = 0;             // the operator code
    int params = 0;         // paramsRequired
    int peek = 0;           // peekParameters: numbers before them it reads without taking them (%)
    int precedence = 0;
};

// Operator codes (Functions.xml)
enum Op {
    Clear = 4, Equals = 7, Plus = 8, Minus = 9, Times = 10, Divide = 11, Power = 12, Sqrt = 13, LeftParen = 14,
    RightParen = 15, Negate = 16, Percent = 17, Squared = 18, Cubed = 19, Sin = 20, ASin = 21, Cos = 22, ACos = 23,
    Tan = 24, ATan = 25, Log10 = 26, TenToX = 27, OneOverX = 28, EToX = 29, Ln = 30, Factorial = 31, Sinh = 32,
    ASinh = 33, Cosh = 34, ACosh = 35, XRootY = 36, Tanh = 37, ATanh = 38, Random = 39, MemPlus = 46, MemMinus = 47,
    MemClear = 48, MemRecall = 49, Pi = 50, E = 51, CubeRoot = 52, Log2 = 53, TwoToX = 54,
    // RPNEngine's own (Functions.xml: enter, RPNDrop, RPNRollUp, RPNRollDown, RPNExchange)
    Enter = 6, RPNDrop = 42, RPNRollUp = 43, RPNRollDown = 44, RPNExchange = 45
};

class Engine {
public:
    // Functions.xml (an XML property list: an array of dictionaries)
    bool load(const std::string &functionsXml);
    const Function *function(int op) const;

    // The keys
    void digit(int d);
    void decimalPoint();
    void exponent();       // EE: what is typed next is the power of ten
    void deleteDigit();    // the delete key: the last digit typed
    void negate();         // ±: the number being typed, else the one shown
    void op(int code);     // an operator or a function (Functions.xml's code)
    void number(long double value);   // a number given whole (Paste)
    void clear();

    bool radians = false;
    // RPN (-[BasicAdvancedController setRPNMode:] swaps AlgebraicEngine for RPNEngine, the number
    // shown carried over): the numbers entered go on a stack (enter), the operators take theirs
    // from it
    void setRPN(bool rpn);
    bool isRPN() const { return m_rpn; }
    // the stack, its top (y) first
    std::vector<long double> rpnStack() const;

    // The paper tape (PaperTapeController): setCurrentEquation: the equation evaluated so far, in
    // place of the tape's last line; completeEquationWithResult: "\n=" and the result, then a new
    // equation ("\n\n")
    std::function<void(const std::string &)> onTapeEquation, onTapeResult;
    // _tapeStringForNumber: %.16lg's digits before the point and the decimal places
    int tapePrecision = 12;
    std::string tapeString(long double v) const;
    // what the engine is told to say (SpeechController): the keys' speechName, the totals
    std::function<void(const std::string &)> onSpeakButton, onSpeakTotal;
    // What the LCD shows: the number being typed as typed, else the last result; an error's text
    std::string display(int decimals, bool separators) const;
    long double value() const;   // the number shown
    bool memoryShown() const { return m_memory != 0; }
    bool parenthesesOpen() const { return m_leftParens > 0; }
    bool error() const { return !m_error.empty(); }
    std::function<void()> beep;

private:
    struct Item {
        bool isNumber = true;
        long double value = 0;
        const Function *f = nullptr;
        std::string tape;   // EngineNumber's tapeRepresentation (empty: the number itself)
    };
    std::string tapeOf(const Item &it) const { return it.tape.empty() ? tapeString(it.value) : it.tape; }
    std::string tapeFor(const Function *f, const std::vector<std::string> &args) const;
    void updateTape();   // -[AlgebraicEngine updateTape]
    void rpnToken(int code);   // -[RPNEngine userEnteredToken:]
    void rpnApply(const Function *f);   // -[RPNEngine applyFunction:]
    void enterTyped();   // the number being typed, as a token
    void enterNumber(long double v, const std::string &tape = {});
    void evaluate();     // evaluateTOS
    void immediate(const Function *f);
    long double perform(const Function *f, const std::vector<long double> &args);
    void result(long double v, const std::string &tape = {});   // processOperationResult:
    void fail(const std::string &key);

    std::vector<Function> m_functions;
    std::vector<Item> m_postfix;
    std::vector<const Function *> m_ops;
    // the last token: none, a number, or the operator (as AlgebraicEngine keeps _lastToken)
    enum { NoToken, NumberToken, OperatorToken } m_lastToken = NoToken;
    int m_lastTokenOp = 0;
    bool m_hasLastNumber = false;
    long double m_lastNumber = 0;          // the last operand of an evaluation (= repeats with it)
    const Function *m_lastFunction = nullptr;
    int m_leftParens = 0;
    long double m_memory = 0;
    std::string m_input;                    // the number being typed ("" when none)
    std::string m_exponentInput;            // after EE
    bool m_inExponent = false;
    long double m_shown = 0;
    std::string m_shownTape;   // the shown result's tapeRepresentation
    bool m_rpn = false;
    std::vector<Item> m_stack;              // RPNEngine's mStack, its top first
    bool m_lastIsResult = false;            // mLastNumberIsOperationResult
    std::string m_error;                    // the Localizable.strings key of the error shown
};

// A number as the LCD writes it: at most `decimals` places, no trailing zeros, the thousands
// separated by commas when asked; very large or small ones with an exponent
std::string formatNumber(long double v, int decimals, bool separators);

} // namespace Calculator
