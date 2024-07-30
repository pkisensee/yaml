///////////////////////////////////////////////////////////////////////////////
//
//  Yaml.cpp
//
//  Copyright © Pete Isensee (PKIsensee@msn.com).
//  All rights reserved worldwide.
//
//  Permission to copy, modify, reproduce or redistribute this source code is
//  granted provided the above copyright notice is retained in the resulting 
//  source code.
// 
//  This software is provided "as is" and without any express or implied
//  warranties.
//
///////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <array>
#include <cassert>

#include "yaml.h"

using namespace PKIsensee;

namespace { // anonymous

constexpr size_t kAsciiTableSize = 256;
constexpr size_t kInvalidPos = size_t( -1 );
constexpr size_t kNoLevel = size_t( -1 );
constexpr size_t kMaxScalarStringPrefixForErrorMsg = 12; // leading chars to print on error

enum class TrimTrailingBlanks
{
  No,
  Yes
};

template <typename T>
static bool CharIsIn( char c, const T& validSet )
{
  return std::any_of( validSet.begin(), validSet.end(), [&]( char e ) { return c == e; } );
}

std::string_view ExtractStr( const char* start, const char* end, TrimTrailingBlanks trimTrailingBlanks )
{
  assert( start != nullptr && end != nullptr );
  assert( start <= end );
  std::string_view str( start, static_cast<size_t>(end - start) );
  if( trimTrailingBlanks == TrimTrailingBlanks::Yes )
    str.remove_suffix( str.size() - ( str.find_last_not_of( ' ' ) + 1 ) );
  return str;
}

///////////////////////////////////////////////////////////////////////////////

} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////

Yaml::Special Yaml::GetSpecialChars( std::string_view scalar )
{
  if( scalar.empty() )
    return Yaml::Special(false);

  // If already quoted, ignore
  if( ( scalar.size() > 2 ) &&
      ( scalar.front() == '\'' || scalar.front() == '\"' ) &&
      ( scalar.front() == scalar.back() ) )
    return Yaml::Special(false);

  // Build ASCII index table from scalar. Once the string is scanned, the table
  // indicates what characters were found and the first position where the
  // given character occurred.
  struct Letter
  {
    size_t firstPos = 0;
    bool found = false;
  };
  std::array<Letter, kAsciiTableSize> ascii;
  for( size_t i = 0; i < scalar.size(); ++i )
  {
    // Treat as unsigned 8-bit to ensure characters map to ASCII; 
    // required for unusual chars with high bit set
    uint8_t c = static_cast<uint8_t>( scalar[ i ] );
    if( ascii[ c ].firstPos == 0 )
    {
      ascii[ c ].firstPos = i;
      ascii[ c ].found = true;
    }
  }

  // Any character less than' ' (0x020) or greater than 'z' (0x7A) is unusual
  constexpr char kLowerBound = ' ';
  constexpr char kUpperBound = 'z';

  // Characters in the 0x20 - 0x7A range are also special YAML values:
  constexpr std::array kSpecialChar = {
    '!', '\"', '#', '$', '%', '&', '\'', '*', ',', '-', 
    '/', ':', '<', '=', '>', '?', '@', '[', '\\', ']', '`'
  };

  // Find the special character at the lowest position
  size_t lowestPosition = kInvalidPos;
  size_t firstSingleQuote = kInvalidPos;
  size_t firstDoubleQuote = kInvalidPos;
  for( size_t c = 0; c < ascii.size(); ++c )
  {
    Letter letter = ascii[ c ];
    if( letter.found )
    {
      if( ( c < kLowerBound ) ||
          ( c > kUpperBound ) ||
          ( std::any_of( kSpecialChar.begin(), kSpecialChar.end(), [&]( size_t e ) { return c == e; } ) ) )
      {
        if( letter.firstPos < lowestPosition )
          lowestPosition = letter.firstPos;
      }
      if( c == '\'' )
        firstSingleQuote = letter.firstPos;
      if( c == '"' )
        firstDoubleQuote = letter.firstPos;
    }
  }

  if( lowestPosition == kInvalidPos ) // no special characters
    return Yaml::Special(false);

  Yaml::Special special;
  special.firstSpecialPos = lowestPosition;
  special.firstSingleQuote = firstSingleQuote;
  special.firstDoubleQuote = firstDoubleQuote;
  special.hasSpecialChars = true;
  special.specialChar = scalar[ lowestPosition ];
  return special;
}

///////////////////////////////////////////////////////////////////////////////
//
// Guarantees the result can be embedded in a YAML file; adding quotes if needed

std::string Yaml::CreateSafeScalar( std::string_view scalar )
{
  Yaml::Special special = GetSpecialChars( scalar );
  char quote = '\0';
  if( special.hasSpecialChars )
  {
    // Ensure scalar doesn't have quotes of two different types
    assert( !( special.firstDoubleQuote < kInvalidPos &&
               special.firstSingleQuote < kInvalidPos ) );

    quote = '\''; // default to single character quote
    if( special.firstSingleQuote < kInvalidPos )
      quote = '\"';
  }

  std::string yaml;
  if( quote )
    yaml += quote;
  yaml += scalar;
  if( quote )
    yaml += quote;
  return yaml;
}

std::string Yaml::CreateKeyValue( std::string_view tag, std::string_view scalar )
{
  std::string yaml;
  yaml += tag;
  yaml += ": ";
  yaml += CreateSafeScalar( scalar );
  yaml += '\n';
  return yaml;
}

///////////////////////////////////////////////////////////////////////////////

YamlParser::YamlParser( std::string_view yaml, YamlHandler& handler ) : 
  curr_( yaml.data() ),
  end_( yaml.data() + yaml.size() ),
  yamlHandler_( handler )
{
  yamlStack_.push( Indent{} ); // avoid having to check for empty stack
}

bool YamlParser::Parse()
{
  yamlHandler_.onStartDocument();
  assert( curr_ != nullptr && end_ != nullptr );
  for( ; curr_ < end_; ++curr_, ++col_ )
  {
    if( col_ == 1 ) // handle new line indentation
    {
      auto indent = GetIndent();
      if( indent.level == kNoLevel )
        ;
      else if( indent.level > yamlStack_.top().level )
        Push( indent );
      else while( indent.level < yamlStack_.top().level )
      {
        if( !Pop() )
          return false;
      }
    }
    switch( *curr_ )
    {
    case '-': // serves multiple purposes
      switch( PeekNext() )
      {
      case ' ': // "- " mapping entry
        yamlHandler_.onStartMapping();
        SkipSpaces();
        break;
      case '-': // "---" start of new document
        SkipStartDocument();
        break;
      default:  // "-X" node, e.g. "-1234"
        if( !ParseNode() )
          return false;
        break;
      }
      break;
    case ':': // mapping value
    case ',': // flow collection separator
      SkipSpaces();
      break;
    case '[': // sequence start, e.g. [ one, two, three ]
      completeKeyValuePair_ = true;
      yamlHandler_.onStartSequence();
      SkipSpaces();
      break;
    case ']': // sequence end
      HandleMissingNull();
      yamlHandler_.onEndSequence();
      SkipSpaces();
      break;
    case '{': // mapping start, e.g. { key1: value1, key2 : value2 }
      completeKeyValuePair_ = true;
      yamlHandler_.onStartMapping();
      SkipSpaces();
      break;
    case '}': // mapping end
      HandleMissingNull();
      yamlHandler_.onEndMapping();
      SkipSpaces();
      break;

    case '#': // comment
    case '%': // directive line
      SkipLine();
      break;
    case '\n': // linefeed
      ++line_;
      col_ = 0;
      break;
    case '\r': // carriage return
    case ' ':  // space
      break;
    case '\0': // null character: early out
      end_ = curr_;
      break;
    case '\t': // tab
      return Error( "Avoid tabs in YAML files" );

    // Characters unsupported by this implementation
    case '|':  // literal scalar
    case '>':  // folder block scalar
    case '?':  // mapping key
    case '&':  // node anchor
    case '*':  // alias
    case '!':  // tag handle
    case '@':  // reserved
    case '`':  // reserved
      return Error( std::string( 1, *curr_ ) + std::string( " directive not supported" ) );

    case '\'': // single-quoted scalar
    case '\"': // double-quoted scalar
    default:   // everything else
      if( !ParseNode() )
        return false;
      break;
    }
  }
  while( yamlStack_.size() > 1 )
    Pop();
  yamlHandler_.onEndDocument();
  return true;
}

///////////////////////////////////////////////////////////////////////////////

bool YamlParser::Error( std::string_view errMessage ) const
{
  yamlHandler_.onError( errMessage, line_, col_ );
  return false; // all syntax issues are sufficient to quit
}

void YamlParser::Push( Indent indent )
{
  completeKeyValuePair_ = true;
  yamlStack_.push( indent );
  indent.isSequence ? yamlHandler_.onStartSequence() : yamlHandler_.onStartMapping();
}

bool YamlParser::Pop()
{
  if( yamlStack_.size() == 1 )
    return Error( "Too many closing braces or brackets" );
  HandleMissingNull();
  yamlStack_.top().isSequence ? yamlHandler_.onEndSequence() : yamlHandler_.onEndMapping();
  yamlStack_.pop();
  return true;
}

char YamlParser::PeekNext() const
{
  return ( curr_ + 1 >= end_ ) ? '\0' : *( curr_ + 1 );
}

YamlParser::Indent YamlParser::GetIndent()
{
  // Skip all leading spaces and dashes to determine indentation level
  constexpr std::array kIndentChars = { ' ', '-' };
  Indent indent;
  for( ; curr_ < end_ && CharIsIn( *curr_, kIndentChars ); ++curr_, ++indent.level )
  {
    if( *curr_ == '-' )
      indent.isSequence = true;
  }

  // If this line doesn't have anything interesting because it's empty or
  // just a comment, then flag it to be ignored
  constexpr std::array kIgnoreIndent = { '\r', '\n', '#' };
  if( CharIsIn( *curr_, kIgnoreIndent ) )
    indent.level = kNoLevel;

  return indent;
}

bool YamlParser::SkipStartDocument()
{
  // Three dashes --- signifies the start of a new YAML doc
  // This implementation ignores multiple YAML docs within a single parsing segment
  auto dashCount = 1;
  for( ++curr_; ( curr_ < end_ ) && ( *curr_ == '-' ) && ( dashCount < 3 ); ++curr_, ++dashCount )
    ;
  col_ += dashCount;
  return dashCount == 3;
}

void YamlParser::SkipSpaces()
{
  for( ++curr_; curr_ < end_ && *curr_ == ' '; ++curr_, ++col_ )
    ;
  --curr_;
  --col_;
}

void YamlParser::SkipLine()
{
  constexpr std::array kEndLine = { '\r', '\n' };
  for( ; curr_ < end_; ++curr_ )
  {
    if( CharIsIn( *curr_, kEndLine ) )
    {
      --curr_;
      break;
    }
  }
}

void YamlParser::HandleMissingNull()
{
  if( !completeKeyValuePair_ )
  {
    yamlHandler_.onScalar( "null" );
    completeKeyValuePair_ = true;
  }
}

bool YamlParser::IsNormalChar() const
{
  // Colons and commas are only special YAML characters when they are 
  // followed by a space. If not, then treat them as part of the token
  constexpr std::array kIsWhite = { ' ', '\r', '\n', '\0' };
  switch( *curr_ )
  {
  case ':':
  case ',':
    if( !CharIsIn( PeekNext(), kIsWhite ) )
      return true;
    [[fallthrough]];
  default:
    return false;
  }
}

bool YamlParser::ParseNode()
{
  switch( *curr_ )
  {
  case '\'': return ParseQuoted( '\'' );
  case '\"': return ParseQuoted( '\"' );
  default:   return ParsePlain();
  }
}

bool YamlParser::ParsePlain() // Unquoted scalar
{
  // Note: order is important; check for comma first
  constexpr std::array kEndScalar = { ',', ':', '\t', '\r', '\n', ']', '}', '#' };
  auto startStr = curr_;
  for( ; curr_ < end_; ++curr_ ) // find end of scalar
  {
    if( CharIsIn( *curr_, kEndScalar ) ) // potential end
    {
      if( IsNormalChar() )
        continue;

      std::string_view str = ExtractStr( startStr, curr_, TrimTrailingBlanks::Yes );
      col_ += curr_ - startStr;
      return OutputScalar( str );
    }
  }
  // End of the file
  completeKeyValuePair_ = true;
  return yamlHandler_.onScalar( ExtractStr( startStr, curr_, TrimTrailingBlanks::Yes ) );
}

bool YamlParser::ParseQuoted(char quote)
{
  constexpr auto kQuoteChars = 2;

  // skip starting quote
  auto startStr = ++curr_;
  for( ; curr_ < end_; ++curr_ ) // find end of scalar
  {
    if ( *curr_ == quote ) // found the end
    {
      std::string_view str = ExtractStr( startStr, curr_, TrimTrailingBlanks::No );

      // Skip to next important character to know if this is a key or value
      static constexpr std::array kImportantChar = { ':', '\t', '\r', '\n', ',', ']', '}', '#' };
      for( ++curr_; curr_ < end_; ++curr_ )
        if( CharIsIn( *curr_, kImportantChar ) )
          break;

      col_ += curr_ - startStr + kQuoteChars;
      return OutputScalar( str );
    }
  }
  // End of the YAML but still inside unterminated quoted string
  // Print out the first few characters of the quoted scalar
  std::string errMessage( "Unterminated quoted scalar <" );
  auto endStr = std::min( curr_, startStr + kMaxScalarStringPrefixForErrorMsg );
  std::string_view str = ExtractStr( startStr-1, endStr, TrimTrailingBlanks::No );
  errMessage += str;
  errMessage += "...>";
  return Error( errMessage );
}

bool YamlParser::OutputScalar( std::string_view str )
{
  // Caller must evaluate the current character, hence --
  if( *curr_-- == ':' ) // key
  {
    HandleMissingNull(); // handle any imcomplete key/value pairs where there's no value
    completeKeyValuePair_ = false;
    return yamlHandler_.onKey( str );
  }
  // else value
  completeKeyValuePair_ = true;
  return yamlHandler_.onScalar( str );
}

///////////////////////////////////////////////////////////////////////////////
