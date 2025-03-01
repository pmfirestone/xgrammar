/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/grammar_parser.cc
 */

#include "grammar_parser.h"

#include <picojson.h>

#include <memory>

#include "grammar_builder.h"
#include "grammar_data_structure.h"
#include "support/encoding.h"

namespace xgrammar {

class EBNFParser {
 public:
  /*! \brief The logic of parsing the grammar string. */
  Grammar Parse(std::string ebnf_string, std::string root_rule_name);

 private:
  using Rule = Grammar::Impl::Rule;

  // Parsing different parts of the grammar
  std::string ParseName(bool accept_empty = false);
  int32_t ParseCharacterClass();
  int32_t ParseString();
  int32_t ParseRuleRef();
  int32_t ParseElement();
  int32_t ParseQuantifier();
  int32_t ParseLookaheadAssertion();
  int32_t ParseSequence();
  int32_t ParseChoices();
  Rule ParseRule();

  // Helper functions
  // Helper for ParseQuantifier
  int32_t HandleStarQuantifier(int32_t rule_expr_id);
  int32_t HandlePlusQuantifier(int32_t rule_expr_id);
  int32_t HandleQuestionQuantifier(int32_t rule_expr_id);

  // When parsing, we first find the names of all rules, and build the mapping from name to rule id.
  void BuildRuleNameToId();
  // Consumes several spaces (newline, space, tab, comment, etc.)
  void ConsumeSpace(bool allow_newline = true);
  // Check the validity of a name
  static bool IsNameChar(TCodepoint c, bool first_char = false);
  // Reset the parser to the beginning of the string.
  void ResetStringIterator(const char* cur);

  // Consume a specified number of characters, and maintain the line and column number.
  void Consume(int cnt = 1) {
    for (int i = 0; i < cnt; ++i) {
      // \n \r \r\n
      if (Peek() == '\n' || (Peek() == '\r' && Peek(1) != '\n')) {
        ++cur_line_;
        cur_column_ = 1;
      } else {
        ++cur_column_;
      }
      ++cur_;
    }
  }

  // Peek the next character.
  char Peek(int delta = 0) const { return *(cur_ + delta); }

  // Report a parsing error with the given message and the line and column number.
  [[noreturn]] void ReportParseError(const std::string& msg) {
    XGRAMMAR_LOG(FATAL) << "EBNF parse error at line " + std::to_string(cur_line_) + ", column " +
                               std::to_string(cur_column_) + ": " + msg;
  }

  // The grammar builder
  GrammarBuilder builder_;
  // A pointer to the current parse position in the string
  const char* cur_ = nullptr;
  // The current line and column number
  int cur_line_ = 1;
  int cur_column_ = 1;
  // The current rule name. Help to generate a name for a new rule.
  std::string cur_rule_name_;
  // Whether the current element is in parentheses.
  // A sequence expression cannot contain newline, unless it is in parentheses.
  bool in_parentheses_ = false;
};

void EBNFParser::ConsumeSpace(bool allow_newline) {
  while (Peek() && (Peek() == ' ' || Peek() == '\t' || Peek() == '#' ||
                    (allow_newline && (Peek() == '\n' || Peek() == '\r')))) {
    Consume();
    if (Peek(-1) == '#') {
      while (Peek() && Peek() != '\n' && Peek() != '\r') {
        Consume();
      }
      if (!Peek()) {
        return;
      }
      Consume();
      if (Peek(-1) == '\r' && Peek() == '\n') {
        Consume();
      }
    }
  }
}

bool EBNFParser::IsNameChar(TCodepoint c, bool first_char) {
  return c == '_' || c == '-' || c == '.' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (!first_char && c >= '0' && c <= '9');
}

// name should be a char string (not a utf8 string)
std::string EBNFParser::ParseName(bool accept_empty) {
  auto start = cur_;
  bool first_char = true;
  while (Peek() && IsNameChar(Peek(), first_char)) {
    Consume();
    first_char = false;
  }
  if (start == cur_ && !accept_empty) {
    ReportParseError("Expect rule name");
  }
  return std::string(start, cur_);
}

// Character class:
// 1. Examples: [a-z] [ab] [a-zA-Z0-9] [^a-z] [测] [\u0123]
// 2. The "-" character is treated as a literal character if it is the last or the first (after
// the "^"", if present) character within the brackets. E.g. [a-] and [-a] means "a" or "-"
// 3. "-" and "]" should be escaped when used as a literal character:
// [\-] means -
// [\]] means ]
// Character class should not contain newlines.
int32_t EBNFParser::ParseCharacterClass() {
  static constexpr TCodepoint kUnknownUpperBound = -4;
  static const std::unordered_map<char, TCodepoint> CUSTOM_ESCAPE_MAP = {{'-', '-'}, {']', ']'}};

  std::vector<GrammarBuilder::CharacterClassElement> elements;

  bool is_negated = false;
  if (Peek() == '^') {
    is_negated = true;
    Consume();
  }

  bool past_is_hyphen = false;
  bool past_is_single_char = false;
  while (Peek() && Peek() != ']') {
    if (Peek() == '\r' || Peek() == '\n') {
      ReportParseError("Character class should not contain newline");
    } else if (Peek() == '-' && Peek(1) != ']' && !past_is_hyphen && past_is_single_char) {
      Consume();
      past_is_hyphen = true;
      past_is_single_char = false;
      continue;
    }

    auto [codepoint, len] = ParseNextUTF8OrEscaped(cur_, CUSTOM_ESCAPE_MAP);
    if (codepoint == CharHandlingError::kInvalidUTF8) {
      ReportParseError("Invalid UTF8 sequence");
    }
    if (codepoint == CharHandlingError::kInvalidEscape) {
      ReportParseError("Invalid escape sequence");
    }
    Consume(len);
    if (past_is_hyphen) {
      XGRAMMAR_ICHECK(!elements.empty());
      if (elements.back().lower > codepoint) {
        ReportParseError("Invalid character class: lower bound is larger than upper bound");
      }
      elements.back().upper = codepoint;
      past_is_hyphen = false;
      XGRAMMAR_ICHECK(past_is_single_char == false);
    } else {
      elements.push_back({codepoint, kUnknownUpperBound});
      past_is_single_char = true;
    }
  }

  for (auto& element : elements) {
    if (element.upper == kUnknownUpperBound) {
      element.upper = element.lower;
    }
  }

  return builder_.AddCharacterClass(elements, is_negated);
}

// parse a c style string with utf8 support
int32_t EBNFParser::ParseString() {
  std::vector<int32_t> codepoints;
  while (Peek() && Peek() != '\"') {
    if (Peek() == '\r' || Peek() == '\n') {
      ReportParseError("There should be no newline character in a string literal");
    }

    auto [codepoint, len] = ParseNextUTF8OrEscaped(cur_);
    if (codepoint == CharHandlingError::kInvalidUTF8) {
      ReportParseError("Invalid utf8 sequence");
    }
    if (codepoint == CharHandlingError::kInvalidEscape) {
      ReportParseError("Invalid escape sequence");
    }
    Consume(len);
    codepoints.push_back(codepoint);
  }
  if (codepoints.empty()) {
    return builder_.AddEmptyStr();
  }

  // convert codepoints to string
  std::string str;
  for (auto codepoint : codepoints) {
    str += PrintAsUTF8(codepoint);
  }
  // convert str to int32_t vector
  std::vector<int32_t> bytes;
  for (auto c : str) {
    bytes.push_back(static_cast<int32_t>(static_cast<uint8_t>(c)));
  }
  return builder_.AddByteString(bytes);
}

int32_t EBNFParser::ParseRuleRef() {
  std::string name = ParseName();
  auto rule_id = builder_.GetRuleId(name);
  if (rule_id == -1) {
    ReportParseError("Rule \"" + name + "\" is not defined");
  }
  return builder_.AddRuleRef(rule_id);
}

int32_t EBNFParser::ParseElement() {
  switch (Peek()) {
    case '(': {
      Consume();
      ConsumeSpace();
      auto prev_in_parentheses = in_parentheses_;
      in_parentheses_ = true;
      auto rule_expr_id = ParseChoices();
      ConsumeSpace();
      if (Peek() != ')') {
        ReportParseError("Expect )");
      }
      Consume();
      in_parentheses_ = prev_in_parentheses;
      return rule_expr_id;
    }
    case '[': {
      Consume();
      auto rule_expr_id = ParseCharacterClass();
      if (Peek() != ']') {
        ReportParseError("Expect ]");
      }
      Consume();
      return rule_expr_id;
    }
    case '\"': {
      Consume();
      auto rule_expr_id = ParseString();
      if (Peek() != '\"') {
        ReportParseError("Expect \"");
      }
      Consume();
      return rule_expr_id;
    }
    default: {
      if (IsNameChar(Peek(), true)) {
        return ParseRuleRef();
      }
      ReportParseError("Expect element");
    }
  }
}

int32_t EBNFParser::HandleStarQuantifier(int32_t rule_expr_id) {
  Grammar::Impl::RuleExpr rule_expr = builder_.GetRuleExpr(rule_expr_id);
  if (rule_expr.type == GrammarBuilder::RuleExprType::kCharacterClass) {
    // We have special handling for character class star, e.g. [a-z]*
    rule_expr.type = GrammarBuilder::RuleExprType::kCharacterClassStar;
    return builder_.AddRuleExpr(rule_expr);
  } else {
    // For other star quantifiers, we transform it into a rule:
    // a*  -->  rule ::= a rule | ""
    auto new_rule_name = builder_.GetNewRuleName(cur_rule_name_);
    auto new_rule_id = builder_.AddEmptyRule(new_rule_name);
    auto ref_to_new_rule = builder_.AddRuleRef(new_rule_id);
    auto new_rule_expr_id = builder_.AddChoices(
        {builder_.AddSequence({rule_expr_id, ref_to_new_rule}), builder_.AddEmptyStr()}
    );
    builder_.UpdateRuleBody(new_rule_id, new_rule_expr_id);

    // Return the reference to the new rule
    return builder_.AddRuleRef(new_rule_id);
  }
}

int32_t EBNFParser::HandlePlusQuantifier(int32_t rule_expr_id) {
  // a+  -->  rule ::= a rule | a
  auto new_rule_name = builder_.GetNewRuleName(cur_rule_name_);
  auto new_rule_id = builder_.AddEmptyRule(new_rule_name);
  auto ref_to_new_rule = builder_.AddRuleRef(new_rule_id);
  auto new_rule_expr_id =
      builder_.AddChoices({builder_.AddSequence({rule_expr_id, ref_to_new_rule}), rule_expr_id});
  builder_.UpdateRuleBody(new_rule_id, new_rule_expr_id);

  // Return the reference to the new rule
  return builder_.AddRuleRef(new_rule_id);
}

int32_t EBNFParser::HandleQuestionQuantifier(int32_t rule_expr_id) {
  // a?  -->  rule ::= a | empty
  auto new_rule_name = builder_.GetNewRuleName(cur_rule_name_);
  auto new_rule_expr_id = builder_.AddChoices({rule_expr_id, builder_.AddEmptyStr()});
  auto new_rule_id = builder_.AddRule({new_rule_name, new_rule_expr_id});
  return builder_.AddRuleRef(new_rule_id);
}

int32_t EBNFParser::ParseQuantifier() {
  int32_t rule_expr_id = ParseElement();
  ConsumeSpace(in_parentheses_);
  if (Peek() != '*' && Peek() != '+' && Peek() != '?') {
    return rule_expr_id;
  }
  Consume();

  // We will transform a*, a+, a? into a rule, and return the reference to this rule
  switch (Peek(-1)) {
    case '*':
      // We assume that the star quantifier should be the body of some rule now
      return HandleStarQuantifier(rule_expr_id);
    case '+':
      return HandlePlusQuantifier(rule_expr_id);
    case '?':
      return HandleQuestionQuantifier(rule_expr_id);
    default:
      XGRAMMAR_LOG(FATAL) << "Unreachable";
  }
}

int32_t EBNFParser::ParseSequence() {
  std::vector<int32_t> elements;
  do {
    elements.push_back(ParseQuantifier());
    ConsumeSpace(in_parentheses_);
  } while (Peek() && Peek() != '|' && Peek() != ')' && Peek() != '\n' && Peek() != '\r' &&
           (Peek() != '(' || Peek(1) != '='));
  return builder_.AddSequence(elements);
}

int32_t EBNFParser::ParseChoices() {
  std::vector<int32_t> choices;

  choices.push_back(ParseSequence());
  ConsumeSpace();
  while (Peek() == '|') {
    Consume();
    ConsumeSpace();
    choices.push_back(ParseSequence());
    ConsumeSpace();
  }
  return builder_.AddChoices(choices);
}

int32_t EBNFParser::ParseLookaheadAssertion() {
  if (Peek() != '(' || Peek(1) != '=') {
    return -1;
  }
  Consume(2);
  auto prev_in_parentheses = in_parentheses_;
  in_parentheses_ = true;
  ConsumeSpace(in_parentheses_);
  auto result = ParseSequence();
  ConsumeSpace(in_parentheses_);
  if (Peek() != ')') {
    ReportParseError("Expect )");
  }
  Consume();
  in_parentheses_ = prev_in_parentheses;
  return result;
}

EBNFParser::Rule EBNFParser::ParseRule() {
  std::string name = ParseName();
  cur_rule_name_ = name;
  ConsumeSpace();
  if (Peek() != ':' || Peek(1) != ':' || Peek(2) != '=') {
    ReportParseError("Expect ::=");
  }
  Consume(3);
  ConsumeSpace();
  auto body_id = ParseChoices();
  ConsumeSpace();
  auto lookahead_id = ParseLookaheadAssertion();
  return {name, body_id, lookahead_id};
}

void EBNFParser::BuildRuleNameToId() {
  ConsumeSpace();
  while (Peek()) {
    auto name = ParseName(true);
    ConsumeSpace(false);
    if (Peek() == ':' && Peek(1) == ':' && Peek(2) == '=') {
      if (name.empty()) {
        ReportParseError("Expect rule name");
      }
      Consume(3);
      if (builder_.GetRuleId(name) != -1) {
        ReportParseError("Rule \"" + name + "\" is defined multiple times");
      }
      builder_.AddEmptyRule(name);
    }
    while (Peek() && Peek() != '\n' && Peek() != '\r') {
      Consume();
    }
    ConsumeSpace();
  }
}

void EBNFParser::ResetStringIterator(const char* cur) {
  cur_ = cur;
  cur_line_ = 1;
  cur_column_ = 1;
  cur_rule_name_ = "";
  in_parentheses_ = false;
}

Grammar EBNFParser::Parse(std::string ebnf_string, std::string root_rule_name) {
  ResetStringIterator(ebnf_string.c_str());
  BuildRuleNameToId();

  ResetStringIterator(ebnf_string.c_str());
  ConsumeSpace();
  while (Peek()) {
    // Throw error when there are multiple lookahead assertions
    if (Peek() == '(' && Peek(1) == '=') {
      ReportParseError("Unexpected lookahead assertion");
    }
    auto new_rule = ParseRule();
    builder_.UpdateRuleBody(new_rule.name, new_rule.body_expr_id);
    // Update the lookahead assertion
    builder_.AddLookaheadAssertion(new_rule.name, new_rule.lookahead_assertion_id);

    ConsumeSpace();
  }

  // Check that the root rule is defined
  if (builder_.GetRuleId(root_rule_name) == -1) {
    ReportParseError("The root rule with name \"" + root_rule_name + "\" is not found.");
  }

  return builder_.Get(root_rule_name);
}

Grammar ParseEBNF(std::string ebnf_string, std::string root_rule_name) {
  EBNFParser parser;
  return parser.Parse(ebnf_string, root_rule_name);
}

}  // namespace xgrammar
